// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Feature Item 3: OSR Entry (Frame State Migration) Test Suite
//
// Design references:
//   - docs/design/hot-loop-osr/【功能设计】基于热循环的OSR能力.md V7.0 (Feature Item 3)
//   - docs/design/hot-loop-osr/【详细设计】OSR进入（帧状态迁移）.md V1.6
//
// Sub-module A: OSRState & OSRMetadata data structures
//   Tests construct and verify OSR data structures.
//   SR coverage: SR-OSR-009, SR-OSR-014, SR-OSR-015
//
// Sub-module B: performOSR three-state return contract
//   Tests verify performOSR's preflight checks (rc=0 frame invariants),
//   non-live-in cleanup (deferred DECREF), and stub interaction.
//   SR coverage: SR-OSR-010, SR-OSR-013, SR-OSR-018
//
// Sub-module C: OSR entry stub code generation
//   Tests verify stub prologue, Environ VReg setup, live-in steal.
//   SR coverage: SR-OSR-011, SR-OSR-012
//
// Sub-module D: Register reservation
//   Tests verify OSR_STUB_SCRATCH_REGS definition.
//   SR coverage: SR-OSR-017
//
// Sub-module E: OSR deopt path verification
//   Tests verify OSR JIT code shares standard kNormal deopt path.
//   SR coverage: SR-OSR-016
//
// Sub-module F: Python-level integration tests
//   Tests run Python code exercising OSR entry. No OSR headers needed.
//
// Build note:
//   This file requires cinderx/Jit/osr.h and cinderx/Jit/osr_capi.h
//   (created as part of Feature Item 1). Until those headers exist,
//   CINDERX_OSR_HEADERS_AVAILABLE will be 0 and all guarded tests will
//   be disabled. Sub-module F tests are NOT guarded and run always.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "cinderx/python.h"

// clang-format off
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
// clang-format on

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

// ---------------------------------------------------------------------------
// Header availability detection.
// osr.h / osr_capi.h are created by Feature Item 1.  When they become
// available, flip CINDERX_OSR_HEADERS_AVAILABLE to 1 (or remove the guard
// and always include them).
// ---------------------------------------------------------------------------
#ifndef CINDERX_OSR_HEADERS_AVAILABLE
#define CINDERX_OSR_HEADERS_AVAILABLE 1
#endif

#if CINDERX_OSR_HEADERS_AVAILABLE
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/osr_capi.h"

#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/codegen/arch/aarch64.h"
#include "cinderx/Jit/frame.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_stackref.h"
#endif

using jit::Compiler;
using jit::getContext;

namespace jit {
void syncOSRFlags();
} // namespace jit

#if CINDERX_OSR_HEADERS_AVAILABLE
using jit::BCOffset;
using jit::CompilationKey;
using jit::CompiledFunction;
using jit::CompiledFunctionData;
using jit::CompilerContext;
using jit::OSRLiveIn;
using jit::OSRMetadata;
using jit::OSRState;
using jit::collectBackedgeTargetOffsets;
using jit::codegen::OSR_STUB_SCRATCH_REGS;
using jit::codegen::PhyRegisterSet;
namespace arch = jit::codegen;

namespace {

using OsrEntryFn = PyObject* (*)(OSRState*);

struct StubObservation {
  bool called{false};
  _Py_CODEUNIT* instr_ptr{nullptr};
  bool non_live_slot_was_null{false};
  bool live_slot_was_null{false};
  Py_ssize_t observed_refcnt{-1};
};

StubObservation g_stub_observation;
int g_live_slot_index{-1};
int g_non_live_slot_index{-1};
PyObject* g_observed_refcnt_obj{nullptr};
int g_dealloc_sets_error_count{0};

void resetStubObservation() {
  g_stub_observation = StubObservation{};
  g_live_slot_index = -1;
  g_non_live_slot_index = -1;
  g_observed_refcnt_obj = nullptr;
  g_dealloc_sets_error_count = 0;
}

PyObject* observingReturnStub(OSRState* state) {
  g_stub_observation.called = true;
  g_stub_observation.instr_ptr = state->frame->instr_ptr;
  if (g_non_live_slot_index >= 0) {
    g_stub_observation.non_live_slot_was_null =
        PyStackRef_IsNull(state->frame->localsplus[g_non_live_slot_index]);
  }
  if (g_live_slot_index >= 0) {
    g_stub_observation.live_slot_was_null =
        PyStackRef_IsNull(state->frame->localsplus[g_live_slot_index]);
  }
  if (g_observed_refcnt_obj != nullptr) {
    g_stub_observation.observed_refcnt = Py_REFCNT(g_observed_refcnt_obj);
  }
  return Py_NewRef(Py_None);
}

PyObject* observingExceptionStub(OSRState* state) {
  g_stub_observation.called = true;
  g_stub_observation.instr_ptr = state->frame->instr_ptr;
  PyErr_SetString(PyExc_ValueError, "osr test stub failed");
  return nullptr;
}

void deallocSetsError(PyObject* self) {
  g_dealloc_sets_error_count++;
  PyErr_SetString(PyExc_RuntimeError, "dealloc clobbered exception");
  Py_TYPE(self)->tp_free(self);
}

Ref<> makeDeallocSetsErrorObject() {
  PyType_Slot slots[] = {
      {Py_tp_dealloc, reinterpret_cast<void*>(deallocSetsError)},
      {Py_tp_new, reinterpret_cast<void*>(PyType_GenericNew)},
      {0, nullptr},
  };
  PyType_Spec spec{
      "runtime_tests.DeallocSetsError",
      sizeof(PyObject),
      0,
      Py_TPFLAGS_DEFAULT,
      slots,
  };
  auto type = Ref<>::steal(PyType_FromSpec(&spec));
  if (type == nullptr) {
    return nullptr;
  }
  return Ref<>::steal(PyObject_CallNoArgs(type));
}

Ref<CompiledFunction> makeStubCompiledFunction(OsrEntryFn stub) {
  CompiledFunctionData data;
  data.code = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(reinterpret_cast<uintptr_t>(stub)),
      1);
  return CompiledFunction::create(std::move(data), true);
}

OSRMetadata makeMetadata(BCOffset target_offset) {
  OSRMetadata metadata;
  metadata.target_offset = target_offset;
  metadata.entry_point_offset = 0;
  return metadata;
}

OSRLiveIn makeLiveIn(int localsplus_index) {
  OSRLiveIn live_in;
  live_in.localsplus_index = localsplus_index;
  live_in.stack_index = -1;
  live_in.destination = arch::X0;
  live_in.reconstructible = true;
  return live_in;
}

BorrowedRef<PyCodeObject> codeFromFunc(BorrowedRef<PyFunctionObject> func) {
  return reinterpret_cast<PyCodeObject*>(PyFunction_GET_CODE(func));
}

class InterpreterFrameHolder {
 public:
  explicit InterpreterFrameHolder(BorrowedRef<PyFunctionObject> func)
      : tstate_{PyThreadState_Get()},
        previous_{currentFrame(tstate_)},
        func_{func},
        code_{codeFromFunc(func)} {
    frame_ = Cix_PyThreadState_PushFrame(tstate_, code_->co_framesize);
    JIT_CHECK(frame_ != nullptr, "failed to push test interpreter frame");
    jit::jitFrameInit(
        tstate_,
        frame_,
        func_,
        code_,
        0,
        FRAME_OWNED_BY_THREAD,
        previous_,
        jit::makeFrameReifier(code_));
    setCurrentFrame(tstate_, frame_);
    frame_->instr_ptr = _PyCode_CODE(code_);
    frame_->stackpointer = _PyFrame_Stackbase(frame_);
  }

  ~InterpreterFrameHolder() {
    if (frame_ == nullptr) {
      return;
    }
    if (currentFrame(tstate_) == frame_) {
      setCurrentFrame(tstate_, frame_->previous);
    }
    jit::jitFrameClearExceptCode(frame_);
    Cix_PyThreadState_PopFrame(tstate_, frame_);
  }

  _PyInterpreterFrame* get() const {
    return frame_;
  }

  void setLocalNewRef(int index, BorrowedRef<> value) {
    PyStackRef_CLOSE(frame_->localsplus[index]);
    frame_->localsplus[index] = PyStackRef_FromPyObjectNew(value);
  }

  void setLocalSteal(int index, PyObject* value) {
    PyStackRef_CLOSE(frame_->localsplus[index]);
    frame_->localsplus[index] = PyStackRef_FromPyObjectSteal(value);
  }

 private:
  PyThreadState* tstate_;
  _PyInterpreterFrame* previous_;
  BorrowedRef<PyFunctionObject> func_;
  BorrowedRef<PyCodeObject> code_;
  _PyInterpreterFrame* frame_{nullptr};
};

Ref<PyFunctionObject> compileSingleLoop(RuntimeTest& test) {
  return Ref<PyFunctionObject>(test.compileAndGet(
      R"(
def test():
    live = 1
    dead = []
    while live < 4:
        live += 1
    return live
)",
      "test"));
}

BCOffset firstBackedgeTarget(BorrowedRef<PyCodeObject> code) {
  auto targets = collectBackedgeTargetOffsets(code);
  JIT_CHECK(!targets.empty(), "test function must have a backedge");
  return targets.front();
}

int performWithMetadata(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    const OSRMetadata& metadata,
    BorrowedRef<CompiledFunction> compiled,
    PyObject** out_result) {
  return jit::performOSR(tstate, frame, &metadata, compiled.get(), out_result);
}

#if defined(__aarch64__)

Ref<CompiledFunction> compileToCompiledFunction(
    BorrowedRef<PyFunctionObject> func) {
  auto data = Compiler().Compile(func);
  if (!data.has_value()) {
    return nullptr;
  }

  CompilationKey key{
      codeFromFunc(func), func->func_builtins, func->func_globals};
  auto* jit_ctx = reinterpret_cast<CompilerContext<Compiler>*>(
      cinderx::getModuleState()->jit_context.get());
  return jit_ctx->makeCompiledFunction(func, key, std::move(*data));
}

uint32_t loadInstructionWord(const std::byte* code) {
  uint32_t word;
  std::memcpy(&word, code, sizeof(word));
  return word;
}

bool codeContainsWord(
    std::span<const std::byte> code,
    std::size_t start,
    uint32_t expected,
    std::size_t max_bytes = 256) {
  std::size_t end = std::min(code.size(), start + max_bytes);
  for (std::size_t offset = start; offset + sizeof(uint32_t) <= end;
       offset += sizeof(uint32_t)) {
    if (loadInstructionWord(code.data() + offset) == expected) {
      return true;
    }
  }
  return false;
}

uint32_t strX12FromX9UnsignedOffset(int32_t byte_offset) {
  JIT_CHECK(byte_offset % 8 == 0, "64-bit STR immediate must be 8-byte scaled");
  return 0xF9000000u | (static_cast<uint32_t>(byte_offset / 8) << 10) |
      (9u << 5) | 12u;
}

uint32_t strW12FromX9UnsignedOffset(int32_t byte_offset) {
  JIT_CHECK(byte_offset % 4 == 0, "32-bit STR immediate must be 4-byte scaled");
  return 0xB9000000u | (static_cast<uint32_t>(byte_offset / 4) << 10) |
      (9u << 5) | 12u;
}

int32_t localsplusOffsetForTest(int localsplus_index) {
  return static_cast<int32_t>(
      offsetof(_PyInterpreterFrame, localsplus) +
      localsplus_index * sizeof(_PyStackRef));
}

#endif // __aarch64__

} // namespace
#endif

// ===========================================================================
// Sub-module A: OSRState & OSRMetadata data structure tests
// ===========================================================================
//
// These tests directly construct and verify OSR data structures.
// SR coverage: SR-OSR-009 (OSRState), SR-OSR-014 (CodeRuntime storage),
//              SR-OSR-015 (entry_point_offset, has_osr_entries)

#if CINDERX_OSR_HEADERS_AVAILABLE

class OSREntryDataTest : public RuntimeTest {};

// TC-A01 (SR-OSR-009): OSRState fields initialized from constructor args.
TEST_F(OSREntryDataTest, OSRState_FieldsInitialized) {
  PyThreadState* tstate = PyThreadState_Get();
  ASSERT_NE(tstate, nullptr);

  // Create a minimal frame to use as test input.
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  BorrowedRef<PyCodeObject> code = PyFunction_GetCode(func);
  _PyInterpreterFrame* frame =
      Cix_PyThreadState_PushFrame(tstate, code->co_framesize);
  ASSERT_NE(frame, nullptr);
  jit::jitFrameInit(
      tstate,
      frame,
      func,
      code,
      0,
      FRAME_OWNED_BY_THREAD,
      nullptr,
      jit::makeFrameReifier(code));

  OSRMetadata meta;
  OSRState state{tstate, frame, &meta};

  EXPECT_EQ(state.tstate, tstate);
  EXPECT_EQ(state.frame, frame);
  EXPECT_EQ(state.osr_meta, &meta);

  // Cleanup: pop the manually-pushed frame.
  Cix_PyThreadState_PopFrame(tstate, frame);
}

// TC-A02 (SR-OSR-009): OSRLiveIn default values.
TEST_F(OSREntryDataTest, OSRLiveIn_Defaults) {
  OSRLiveIn li;

  EXPECT_EQ(li.localsplus_index, -1);
  EXPECT_EQ(li.stack_index, -1);
  EXPECT_TRUE(li.reconstructible);
  EXPECT_FALSE(li.is_phi);
  EXPECT_EQ(li.hir_reg, nullptr);
  // destination should be an uninitialized/default PhyLocation.
  // value_kind and ref_kind are enum types; default initialization
  // depends on the enum definition. We verify they are accessible.
  (void)li.value_kind;
  (void)li.ref_kind;
  (void)li.destination;
}

// TC-A03 (SR-OSR-014/015): OSRMetadata defaults.
TEST_F(OSREntryDataTest, OSRMetadata_Defaults) {
  OSRMetadata meta;

  EXPECT_EQ(meta.entry_point_offset, -1);
  EXPECT_EQ(meta.owned_ref_count, 0);
  EXPECT_EQ(meta.resume_frame_total_size, 0);
  EXPECT_EQ(meta.resume_header_and_spill_size, 0);
  EXPECT_TRUE(meta.live_ins.empty());

  // Environ VReg locations default-constructed.
  (void)meta.tstate_location;
  (void)meta.func_location;
  (void)meta.frame_location;
  (void)meta.target_offset;
  (void)meta.resume_saved_regs;
}

// TC-A04 (SR-OSR-015): entryPoint() returns nullptr for negative offset.
TEST_F(OSREntryDataTest, OSRMetadata_EntryPoint_NegativeOffset) {
  OSRMetadata meta;
  meta.entry_point_offset = -1;

  EXPECT_LT(meta.entry_point_offset, 0);

  // TODO(T-OSR-003): Test entryPoint(compiled) actually returns nullptr
  // when entry_point_offset < 0. Requires Feature Item 2 compilation
  // pipeline to create a real CompiledFunction with codeBuffer().
}

// TC-A05 (SR-OSR-015): entryPoint() returns valid pointer for positive offset.
TEST_F(OSREntryDataTest, OSRMetadata_EntryPoint_ValidOffset) {
  OSRMetadata meta;
  meta.entry_point_offset = 64; // Arbitrary positive offset.

  EXPECT_GE(meta.entry_point_offset, 0);
  // Full entryPoint() test requires a real CompiledFunction with codeBuffer.
  // TODO(T-OSR-003): Test entryPoint() with a real CompiledFunction once
  // Feature Item 2 provides the compilation pipeline.
}

// TC-A06 (SR-OSR-014): allReconstructible() returns true only when all
// live-ins have reconstructible=true.
TEST_F(OSREntryDataTest, OSRMetadata_AllReconstructible) {
  OSRMetadata meta;

  // Empty live-ins → all reconstructible (vacuously true).
  EXPECT_TRUE(meta.allReconstructible());

  // Add a reconstructible live-in.
  OSRLiveIn li1;
  li1.localsplus_index = 0;
  li1.reconstructible = true;
  OSRMetadata reconstructible_meta;
  reconstructible_meta.live_ins = {li1};
  EXPECT_TRUE(reconstructible_meta.allReconstructible());

  // Add a non-reconstructible live-in.
  OSRLiveIn li2;
  li2.localsplus_index = 1;
  li2.reconstructible = false;
  OSRMetadata mixed_meta;
  mixed_meta.live_ins = {li1, li2};
  EXPECT_FALSE(mixed_meta.allReconstructible());
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Sub-module B: performOSR three-state return contract tests
// ===========================================================================
//
// These tests verify performOSR's preflight checks, frame invariants on rc=0,
// non-live-in cleanup (deferred DECREF), and stub interaction.
// SR coverage: SR-OSR-010 (performOSR), SR-OSR-013 (live-in refcount),
//              SR-OSR-018 (preflight timing)
//
// Note: Many tests are structural placeholders until Feature Items 1-2
// provide the full compilation pipeline. Tests that verify preflight
// rejection paths can be fully implemented.

#if CINDERX_OSR_HEADERS_AVAILABLE

class PerformOSRTest : public RuntimeTest {
 protected:
  // Common source for a single while-loop function.
  static constexpr const char* kSingleLoopSrc = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
};

// TC-B01 (SR-OSR-010/018): entry_fn=null → rc=0, frame unchanged.
//
// Design (entry V1.6 section 1.2.1): When osr_meta->entryPoint(compiled)
// returns nullptr (entry_point_offset < 0), performOSR returns 0 without
// modifying the frame. This is the first check in step [0].
//
// TODO(T-OSR-003): Requires Feature Item 2 compilation pipeline to create
// a CompiledFunction with an OSRMetadata whose entry_point_offset < 0.
// Placeholder until then.
TEST_F(PerformOSRTest, EntryPointNull_Returns0_FrameUnchanged) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(live, nullptr);
  frame_holder.setLocalNewRef(0, live);

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.entry_point_offset = -1;
  metadata.live_ins = {makeLiveIn(0)};

  _Py_CODEUNIT* before_instr = frame->instr_ptr;
  uintptr_t before_local = frame->localsplus[0].bits;
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_FALSE(g_stub_observation.called);
  EXPECT_EQ(frame->instr_ptr, before_instr);
  EXPECT_EQ(frame->localsplus[0].bits, before_local);
  EXPECT_EQ(currentFrame(PyThreadState_Get()), frame);
  EXPECT_EQ(result, nullptr);
}

// TC-B02 (SR-OSR-010/018): Live-in slot is PyStackRef_NULL → rc=0.
//
// Design (entry V1.6 section 1.2.1, preflight): performOSR checks each
// live-in's localsplus slot. If any is PyStackRef_NULL, return 0.
// This covers unbound locals after `del` or definite-assignment edge cases.
//
// TODO(T-OSR-003): Requires compilation pipeline to set up OSRMetadata
// with live-in mappings.
TEST_F(PerformOSRTest, LiveInNull_Returns0_Preflight) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_FALSE(g_stub_observation.called);
  EXPECT_EQ(result, nullptr);
}

// TC-B03 (SR-OSR-010/018): localsplus_index >= co_nlocalsplus → rc=0.
//
// Design (entry V1.6 section 1.2.1, preflight): Out-of-bounds index
// indicates metadata inconsistency; reject OSR gracefully.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, LiveInIndexOOB_Returns0_Preflight) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(codeFromFunc(func)->co_nlocalsplus)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_FALSE(g_stub_observation.called);
  EXPECT_EQ(result, nullptr);
}

// TC-B04 (SR-OSR-010/018): Non-empty operand stack → rc=0.
//
// Design (entry V1.6 section 1.2.1, MVP constraint): performOSR checks
// stackpointer == _PyFrame_Stackbase(frame). Non-empty stack means the
// JUMP_BACKWARD_JIT is not a while-loop backedge (could be for-loop
// with iterator on stack).
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, StackNotEmpty_Returns0_Preflight) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(live, nullptr);
  frame_holder.setLocalNewRef(0, live);

  _PyStackRef* stackbase = _PyFrame_Stackbase(frame);
  stackbase[0] = PyStackRef_FromPyObjectNew(Py_None);
  frame->stackpointer = stackbase + 1;

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_FALSE(g_stub_observation.called);
  EXPECT_EQ(result, nullptr);

  PyStackRef_CLOSE(stackbase[0]);
  stackbase[0] = PyStackRef_NULL;
  frame->stackpointer = stackbase;
}

// TC-B05 (SR-OSR-018): rc=0 path does not modify frame->instr_ptr.
//
// Design (entry V1.6 section 1.2.1): All rc=0 returns happen before
// step [0.5] (instr_ptr modification). This is the core preflight timing
// invariant—rc=0 guarantees frame completely unchanged.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, Rc0_NoInstrPtrModification) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  _Py_CODEUNIT* before_instr = frame->instr_ptr;
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_EQ(frame->instr_ptr, before_instr);
}

// TC-B06 (SR-OSR-018): rc=0 path does not modify localsplus.
//
// Design (entry V1.6 section 1.2.1): All rc=0 returns happen before
// step [1] (localsplus collection). Frame invariants guaranteed.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, Rc0_NoLocalsplusModification) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  auto dead = Ref<>::steal(PyList_New(0));
  ASSERT_NE(live, nullptr);
  ASSERT_NE(dead, nullptr);
  frame_holder.setLocalNewRef(0, live);
  frame_holder.setLocalNewRef(1, dead);

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.entry_point_offset = -1;
  metadata.live_ins = {makeLiveIn(0)};
  uintptr_t before_live = frame->localsplus[0].bits;
  uintptr_t before_dead = frame->localsplus[1].bits;
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_EQ(frame->localsplus[0].bits, before_live);
  EXPECT_EQ(frame->localsplus[1].bits, before_dead);
}

// TC-B07 (SR-OSR-018): rc=0 path does not modify frame chain.
//
// Design (entry V1.6 section 1.2.1): tstate->current_frame must be
// identical before and after rc=0 path.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, Rc0_NoFrameChainModification) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.entry_point_offset = -1;
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      0);
  EXPECT_EQ(currentFrame(PyThreadState_Get()), frame);
}

// TC-B08 (SR-OSR-010/018): frame->instr_ptr set to loop header before stub.
//
// Design (entry V1.6 section 1.2.1, step [0.5]): After all preflight
// checks pass, performOSR sets frame->instr_ptr to
// code_start + target_offset / sizeof(_Py_CODEUNIT).
// This happens BEFORE stub call for observability (tracing, sys._getframe).
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, InstrPtrSetBeforeStubCall) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(live, nullptr);
  frame_holder.setLocalNewRef(0, live);

  auto target = firstBackedgeTarget(codeFromFunc(func));
  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(target);
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      1);
  ASSERT_TRUE(g_stub_observation.called);
  EXPECT_EQ(
      g_stub_observation.instr_ptr,
      _PyCode_CODE(codeFromFunc(func)) + target.value() / sizeof(_Py_CODEUNIT));
  Py_XDECREF(result);
}

// TC-B09 (SR-OSR-013): Non-live-in slots cleared to PyStackRef_NULL.
//
// Design (entry V1.6 section 1.2.1, step [1]): performOSR collects
// non-live-in slots, steal values to deferred_decrefs[], writes
// PyStackRef_NULL to each slot. This ensures deopt reifyLocalsplus's
// blind-write assumption holds.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, NonLiveInSlots_ClearedToNull) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  auto dead = Ref<>::steal(PyList_New(0));
  ASSERT_NE(live, nullptr);
  ASSERT_NE(dead, nullptr);
  frame_holder.setLocalNewRef(0, live);
  frame_holder.setLocalNewRef(1, dead);
  g_live_slot_index = 0;
  g_non_live_slot_index = 1;

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      1);
  ASSERT_TRUE(g_stub_observation.called);
  EXPECT_TRUE(g_stub_observation.non_live_slot_was_null);
  EXPECT_FALSE(g_stub_observation.live_slot_was_null);
  EXPECT_TRUE(PyStackRef_IsNull(frame->localsplus[1]));
  Py_XDECREF(result);
}

// TC-B10 (SR-OSR-013): Deferred DECREFs executed after stub returns.
//
// Design (entry V1.6 section 1.2.1, step [3]): performOSR defers all
// DECREFs to after stub returns (JIT epilogue has unlinked F).
// This avoids triggering finalizers while F is still current_frame.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, DeferredDecrefs_AfterStubReturn) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  auto dead = Ref<>::steal(PyList_New(0));
  ASSERT_NE(live, nullptr);
  ASSERT_NE(dead, nullptr);
  frame_holder.setLocalNewRef(0, live);
  frame_holder.setLocalNewRef(1, dead);
  g_observed_refcnt_obj = dead;
  Py_ssize_t before_refcnt = Py_REFCNT(dead);

  auto compiled = makeStubCompiledFunction(observingReturnStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      1);
  EXPECT_EQ(g_stub_observation.observed_refcnt, before_refcnt);
  EXPECT_EQ(Py_REFCNT(dead), before_refcnt - 1);
  Py_XDECREF(result);
}

// TC-B11 (SR-OSR-013): Exception state preserved through deferred DECREF.
//
// Design (entry V1.6 section 1.2.1, step [3]): When stub returns NULL
// (exception), performOSR uses PyErr_GetRaisedException/SetRaisedException
// to protect the current exception from finalizer interference during
// deferred DECREFs.
//
// TODO(T-OSR-003): Requires compilation pipeline.
TEST_F(PerformOSRTest, ExceptionStatePreserved_DuringDECREF) {
  resetStubObservation();
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  InterpreterFrameHolder frame_holder(func);
  auto frame = frame_holder.get();
  auto live = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(live, nullptr);
  frame_holder.setLocalNewRef(0, live);

  auto finalizer = makeDeallocSetsErrorObject();
  ASSERT_NE(finalizer, nullptr);
  frame_holder.setLocalSteal(1, finalizer.release());

  auto compiled = makeStubCompiledFunction(observingExceptionStub);
  ASSERT_NE(compiled, nullptr);
  OSRMetadata metadata = makeMetadata(firstBackedgeTarget(codeFromFunc(func)));
  metadata.live_ins = {makeLiveIn(0)};
  PyObject* result = nullptr;

  EXPECT_EQ(
      performWithMetadata(
          PyThreadState_Get(), frame, metadata, compiled, &result),
      -1);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(g_dealloc_sets_error_count, 1);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
  PyErr_Clear();
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Sub-module C: OSR entry stub code generation tests
// ===========================================================================
//
// These tests verify OSR entry stub prologue, Environ VReg setup, and
// live-in steal code generation.
// SR coverage: SR-OSR-011 (stub prologue), SR-OSR-012 (live-in steal)
//
// All tests require Feature Item 2's compilation pipeline to generate
// actual stub machine code. These are structural placeholders.

#if CINDERX_OSR_HEADERS_AVAILABLE

class OSRStubTest : public RuntimeTest {
 protected:
  void SetUp() override {
    RuntimeTest::SetUp();
    previous_osr_enabled_ = jit::getConfig().osr_enabled;
    jit::getMutableConfig().osr_enabled = true;
    jit::syncOSRFlags();
  }

  void TearDown() override {
    jit::getMutableConfig().osr_enabled = previous_osr_enabled_;
    jit::syncOSRFlags();
    RuntimeTest::TearDown();
  }

 private:
  bool previous_osr_enabled_{false};
};

// TC-C01 (SR-OSR-011): Stub prologue matches kNormal JIT prologue layout.
//
// Design (entry V1.6 section 1.2.2): Stub must replicate kNormal JIT
// prologue's native stack layout exactly (stp fp/lr, mov fp/sp,
// sub sp, save callee-saved). OSRMetadata stores
// resume_frame_total_size, resume_header_and_spill_size, resume_saved_regs
// computed by computeFrameInfo().
//
// TODO(T-OSR-002): Requires Feature Item 2 compilation pipeline.
TEST_F(OSRStubTest, StubPrologue_MatchesNormalLayout) {
#if defined(__aarch64__)
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  Ref<CompiledFunction> compiled = compileToCompiledFunction(func);
  ASSERT_NE(compiled, nullptr);
  const auto& metadatas = compiled->runtime()->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  const OSRMetadata& metadata = metadatas.front();
  ASSERT_GE(metadata.entry_point_offset, 0);
  EXPECT_GT(metadata.resume_frame_total_size, 0);
  EXPECT_GT(metadata.resume_header_and_spill_size, 0);

  const std::byte* entry =
      static_cast<const std::byte*>(metadata.entryPoint(*compiled.get()));
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(loadInstructionWord(entry), 0xA9BF7BFDu); // stp fp, lr, [sp,#-16]!
  EXPECT_EQ(loadInstructionWord(entry + 4), 0x910003FDu); // mov fp, sp
#else
  GTEST_SKIP() << "OSR entry stubs are implemented for aarch64";
#endif
}

// TC-C02 (SR-OSR-011): Environ VRegs set correctly.
//
// Design (entry V1.6 section 1.2.5): Stub writes tstate, frame, func
// to PhyLocations recorded in OSRMetadata by regalloc.
//
// TODO(T-OSR-002): Requires Feature Item 2 compilation pipeline.
TEST_F(OSRStubTest, EnvironVRegs_SetCorrectly) {
#if defined(__aarch64__)
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  Ref<CompiledFunction> compiled = compileToCompiledFunction(func);
  ASSERT_NE(compiled, nullptr);
  const auto& metadatas = compiled->runtime()->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  const OSRMetadata& metadata = metadatas.front();

  EXPECT_NE(metadata.tstate_location.loc, arch::PhyLocation::REG_INVALID);
  EXPECT_NE(metadata.func_location.loc, arch::PhyLocation::REG_INVALID);
  EXPECT_NE(metadata.frame_location.loc, arch::PhyLocation::REG_INVALID);
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(metadata.tstate_location));
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(metadata.func_location));
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(metadata.frame_location));
#else
  GTEST_SKIP() << "OSR entry stubs are implemented for aarch64";
#endif
}

// TC-C03 (SR-OSR-012): Stub writes PyStackRef_NULL (bits=1), not zero.
//
// Design (entry V1.6 section 1.2.2, step 4): After stealing a live-in
// value, stub writes PyStackRef_NULL (bits=1) back to the source slot.
// Must NOT use str xzr (writes 0), as PyStackRef_IsNull checks bits==1.
// Must use 64-bit str (not 32-bit str w), as _PyStackRef is 8-byte union.
//
// TODO(T-OSR-002): Requires Feature Item 2 to generate stub machine code.
TEST_F(OSRStubTest, LiveInSteal_WritesPyStackRefNull) {
#if defined(__aarch64__)
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  Ref<CompiledFunction> compiled = compileToCompiledFunction(func);
  ASSERT_NE(compiled, nullptr);
  const auto& metadatas = compiled->runtime()->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  const OSRMetadata& metadata = metadatas.front();
  ASSERT_FALSE(metadata.live_ins.empty());

  auto code = compiled->codeBuffer();
  std::size_t entry_offset = static_cast<std::size_t>(metadata.entry_point_offset);
  EXPECT_TRUE(codeContainsWord(code, entry_offset, 0xD280002Cu))
      << "expected mov x12, #1 before storing PyStackRef_NULL";
  for (const OSRLiveIn& live_in : metadata.live_ins) {
    int32_t slot_offset = localsplusOffsetForTest(live_in.localsplus_index);
    EXPECT_TRUE(codeContainsWord(
        code, entry_offset, strX12FromX9UnsignedOffset(slot_offset)))
        << "expected 64-bit store of PyStackRef_NULL for localsplus index "
        << live_in.localsplus_index;
  }
#else
  GTEST_SKIP() << "OSR entry stubs are implemented for aarch64";
#endif
}

// TC-C04 (SR-OSR-012): Live-in steal uses 64-bit store.
//
// Design (entry V1.6 section 1.2.2): _PyStackRef is a union with
// uintptr_t bits (8 bytes on aarch64). 32-bit str only writes low 4 bytes,
// leaving high bytes as garbage. PyStackRef_IsNull(bits==1) would fail.
//
// TODO(T-OSR-002): Requires Feature Item 2 to generate stub machine code.
TEST_F(OSRStubTest, LiveInSteal_64BitStore) {
#if defined(__aarch64__)
  static_assert(sizeof(_PyStackRef) == 8, "OSR assumes 8-byte stack refs");
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  Ref<CompiledFunction> compiled = compileToCompiledFunction(func);
  ASSERT_NE(compiled, nullptr);
  const auto& metadatas = compiled->runtime()->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  const OSRMetadata& metadata = metadatas.front();
  ASSERT_FALSE(metadata.live_ins.empty());

  auto code = compiled->codeBuffer();
  std::size_t entry_offset = static_cast<std::size_t>(metadata.entry_point_offset);
  for (const OSRLiveIn& live_in : metadata.live_ins) {
    int32_t slot_offset = localsplusOffsetForTest(live_in.localsplus_index);
    EXPECT_TRUE(codeContainsWord(
        code, entry_offset, strX12FromX9UnsignedOffset(slot_offset)));
    EXPECT_FALSE(codeContainsWord(
        code, entry_offset, strW12FromX9UnsignedOffset(slot_offset)))
        << "OSR live-in steal must not use a 32-bit store";
  }
#else
  GTEST_SKIP() << "OSR entry stubs are implemented for aarch64";
#endif
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Sub-module D: Register reservation tests
// ===========================================================================
//
// Tests verify OSR_STUB_SCRATCH_REGS definition and properties.
// SR coverage: SR-OSR-017

#if CINDERX_OSR_HEADERS_AVAILABLE

// TC-D01 (SR-OSR-017): OSR_STUB_SCRATCH_REGS contains exactly X9 and X12.
//
// Design (entry V1.6 section 1.2.8): Stub uses X9 (persistent state/
// localsplus base) and X12 (temporary/PyStackRef_NULL constant) as
// scratch registers. X10/X11 excluded (INITIAL_EXTRA_ARGS_REG/
// INITIAL_TSTATE_REG).
TEST_F(OSREntryDataTest, StubScratchRegs_AreX9X12) {
  // Verify X9 and X12 are in the set.
  EXPECT_TRUE(OSR_STUB_SCRATCH_REGS.Has(arch::X9));
  EXPECT_TRUE(OSR_STUB_SCRATCH_REGS.Has(arch::X12));

  // Verify X10 and X11 are NOT in the set.
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(arch::X10));
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(arch::X11));

  // Verify no other caller-saved regs are included.
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(arch::X0));
  EXPECT_FALSE(OSR_STUB_SCRATCH_REGS.Has(arch::X1));
}

// TC-D02 (SR-OSR-017): OSR_STUB_SCRATCH_REGS is subset of CALLER_SAVE_REGS.
//
// Design (entry V1.6 section 1.2.8): X9 and X12 are caller-saved registers
// on aarch64. They must not be callee-saved, as stub doesn't need to
// save/restore them.
TEST_F(OSREntryDataTest, StubScratchRegs_SubsetOfCallerSave) {
  PhyRegisterSet remaining = OSR_STUB_SCRATCH_REGS;
  while (!remaining.Empty()) {
    auto reg = remaining.GetFirst();
    remaining.RemoveFirst();
    EXPECT_TRUE(arch::CALLER_SAVE_REGS.Has(reg))
        << "Stub scratch register is not in CALLER_SAVE_REGS";
  }
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Sub-module E: OSR deopt path verification
// ===========================================================================
//
// SR coverage: SR-OSR-016

#if CINDERX_OSR_HEADERS_AVAILABLE

// TC-E01 (SR-OSR-016): OSR JIT code shares standard kNormal deopt path.
//
// Design (function V7.0): OSR JIT code is compiled through the same
// Compiler::Compile() path as normal JIT code. Deopt exits use the same
// stage1→2→3 trampoline chain, same prepareForDeopt, same reifyFrame,
// same resumeInInterpreter. No modifications to deopt code needed.
//
// TODO(T-OSR-002): Requires Feature Item 2 compilation pipeline.
TEST_F(OSRStubTest, OSRDeopt_SharesNormalPath) {
#if defined(__aarch64__)
  Ref<PyFunctionObject> func = compileSingleLoop(*this);
  ASSERT_NE(func, nullptr);
  Ref<CompiledFunction> compiled = compileToCompiledFunction(func);
  ASSERT_NE(compiled, nullptr);
  ASSERT_NE(compiled->runtime(), nullptr);
  EXPECT_TRUE(compiled->hasOSREntries());
  EXPECT_FALSE(compiled->runtime()->osrMetadatas().empty());
  EXPECT_FALSE(compiled->runtime()->deoptMetadatas().empty())
      << "OSR compiled functions should retain normal deopt metadata";
#else
  GTEST_SKIP() << "OSR entry stubs are implemented for aarch64";
#endif
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Sub-module F: Python-level integration tests
// ===========================================================================
//
// These tests run Python code that exercises the full OSR pipeline
// (detect → compile → enter). They do NOT require OSR C++ headers.
// They verify crash safety and basic correctness.
//
// kOwned note: `i += 1` uses InPlaceOp (kOwned output → STORE_FAST),
// so `i` is kOwned at the loop header. `result += n` similarly.
// These patterns ensure OSR entry is not rejected by the kOwned-only
// MVP constraint (ADR-5).

// ---------------------------------------------------------------------------
// OSR integration test fixture — configures OSR with a low backedge threshold
// so integration tests trigger OSR reliably. Matches the pattern used in
// osr_exit_degrade_test.cpp's OSRDeoptTest fixture.
// ---------------------------------------------------------------------------
class OSRIntegrationTest : public RuntimeTest {
 protected:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit::getMutableConfig().osr_enabled = true;
    jit::getMutableConfig().osr_backedge_threshold = 10;
    jit::syncOSRFlags();
  }

  void TearDown() override {
    jit::getMutableConfig().osr_enabled = false;
    jit::getMutableConfig().osr_backedge_threshold = 2000;
    jit::syncOSRFlags();
    RuntimeTest::TearDown();
  }
};

// TC-F01: while loop triggers OSR and produces correct result.
//
// Design (function V7.0, integration tests): MVP primary acceptance
// scenario. A simple counting while loop should trigger OSR after
// the backedge counter reaches threshold (10 in tests), and produce
// the correct result.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_TriggersOSR_CorrectResult) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 1000:
        result += i
        i += 1
    return result

assert test() == sum(range(1000))
)";
  runCode(src);
}

// TC-F02: Nested while loops — inner loop triggers OSR.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, NestedWhile_InnerTriggersOSR) {
  const char* src = R"(
def test():
    total = 0
    i = 0
    while i < 10:
        j = 0
        while j < 100:
            total += j
            j += 1
        i += 1
    return total

assert test() == 10 * sum(range(100))
)";
  runCode(src);
}

// TC-F03: while loop with function calls inside.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_WithFunctionCalls) {
  const char* src = R"(
def add(a, b):
    return a + b

def test():
    result = 0
    i = 0
    while i < 200:
        result = add(result, i)
        i += 1
    return result

assert test() == sum(range(200))
)";
  runCode(src);
}

// TC-F04: while loop with minimal iterations still produces correct result.
//
// This test exercises the case where the loop iteration count is only
// slightly above the test threshold. It verifies computation correctness
// regardless of whether OSR triggers at the exact boundary.
// Deopt-specific tests are in osr_exit_degrade_test.cpp (Feature Item 4).
TEST_F(OSRIntegrationTest, WhileLoop_SmallCounter) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 50:
        result += i
        i += 1
    return result

assert test() == sum(range(50))
)";
  runCode(src);
}

// TC-F05: OSR entry then exception handling in loop body.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_OSRExceptionHandling) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 100:
        try:
            if i == 50:
                raise ValueError("mid")
            result += i
        except ValueError:
            result += 1000
        i += 1
    return result

expected = sum(range(50)) + 1000 + sum(range(51, 100))
assert test() == expected, f"{test()} != {expected}"
)";
  runCode(src);
}

// TC-F06: Multiple iterations of while loop after OSR entry.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_MultipleIterations) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 5000:
        result += i
        i += 1
    return result

assert test() == sum(range(5000))
)";
  runCode(src);
}

// TC-F07: Large counter to exercise hot loop detection.
//
// Note: This test verifies computation correctness. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_LargeCounter) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 100000:
        result += i
        i += 1
    return result

assert test() == sum(range(100000))
)";
  runCode(src);
}

// TC-F08: Refcount balance after OSR entry + exit.
//
// Verify that OSR does not leak or double-free references by checking
// that a function can be called many times without memory issues.
//
// Note: This test verifies refcount balance. It does not verify
// that OSR was actually triggered (would require osr_entry_count check).
TEST_F(OSRIntegrationTest, WhileLoop_RefcountBalance) {
  const char* src = R"(
import gc

def test():
    result = 0
    i = 0
    while i < 500:
        result += i
        i += 1
    return result

# Run multiple times to detect refcount imbalance.
for _ in range(10):
    assert test() == sum(range(500))

# Force garbage collection to detect leaks.
gc.collect()
)";
  runCode(src);
}

// TC-F09: co_nlocalsplus==0 edge case — function with no locals at loop header.
//
// Design (entry V1.6, section 2.1.2): When co_nlocalsplus==0, the collection
// loop in performOSR exits immediately with n_deferred=0, and stub has no
// live-in to steal. Stub does prologue + Environ VReg setup + jmp only.
// This verifies the empty-frame boundary condition.
TEST_F(OSRIntegrationTest, WhileLoop_NoLocals_ZeroNLocalsplus) {
  const char* src = R"(
def test():
    while True:
        break
    return 42

assert test() == 42
)";
  runCode(src);
}

// TC-F10: Empty live_ins at loop header — all locals are dead.
//
// Design (entry V1.6, section 2.1.2): When osr_meta->live_ins is empty
// (no live variables at the loop header), stub skips the steal step
// entirely and jumps directly to the loop header JIT code.
// performOSR still collects non-live-in slots (all of them) for deferred
// DECREF. This verifies the empty-live-in boundary condition.
TEST_F(OSRIntegrationTest, WhileLoop_EmptyLiveIns) {
  const char* src = R"(
def test():
    # x is assigned before the loop but never read inside the loop body.
    # At the loop header, x may be dead (depending on HIR analysis).
    x = 0
    count = 0
    while count < 50:
        count += 1
    return count

assert test() == 50
)";
  runCode(src);
}
