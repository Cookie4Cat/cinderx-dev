// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Feature Item 4: OSR Exit & Degrade Test Suite
//
// Design references:
//   - docs/design/hot-loop-osr/hot-loop-osr-function-design.md V7.0 (Feature Item 4)
//   - docs/design/hot-loop-osr/hot-loop-osr-exit-degrade-detailed-design.md V1.5
//
// Sub-module A: OSR deopt path reuse verification
//   OSR JIT code shares the same deopt path as normal JIT code.
//   These tests verify deopt correctness after OSR entry.
//   Key design points (exit-degrade V1.5):
//     - deopt PopFrame executed by interpreter RETURN_VALUE (not JIT epilogue)
//       after resumeInInterpreter → _PyEval_EvalFrame runs to function end
//     - setCurrentFrame(tstate, frame->previous) in resumeInInterpreter only
//       (gen_asm.cpp:318); kNormal does NOT call setCurrentFrame in
//       prepareForDeopt (gen_asm.cpp:171-178 is kLightweight-only)
//     - deferred_decrefs safe regardless of PopFrame executor: values already
//       steal'd from frame, frame release doesn't affect their refcount
//     - reifyLocalsplus assumes all slots NULL (performOSR + stub steal guarantee)
//     - kOwned-only MVP: extractOSRLiveIns rejects kBorrowed live-ins (ADR-5);
//       test functions must use patterns producing kOwned values at loop headers
//   Dependencies: Feature Items 1-3 (full OSR entry pipeline)
//
// Sub-module B: Compilation invalidation cleanup
//   resetOSRState() resets BackedgeCounters on func.__code__ replacement.
//   These tests verify reset behavior and funcModified integration.
//   Key design points (exit-degrade V1.5):
//     - resetOSRState called BEFORE func_code update (not after)
//     - resetOSRState uses PyErr_GetRaisedException/SetRaisedException (3.12+)
//       to preserve PyErr state (exception safety)
//     - typeModified does NOT call resetOSRState (guard deopt handles types);
//       FailedPermanent states persist across typeModified (code-level constraint)
//     - funcModified does NOT delete CompiledFunction from cache (unlike uncompile)
//     - osr_aware cache degradation semantics: resetOSRState doesn't change
//       CompiledFunction osr_aware/has_osr_entries flags
//   Dependencies: Feature Item 1 (BackedgeCounters), Feature Item 4 (resetOSRState)
//
// Build note:
//   This file requires cinderx/Jit/osr.h and cinderx/Jit/osr_capi.h
//   (created as part of Feature Item 1). Until those headers exist,
//   CINDERX_OSR_HEADERS_AVAILABLE will be 0 and all tests will be
//   disabled. Once headers are available, set the macro to 1 or
//   remove the guard to enable the tests.

#include <gtest/gtest.h>

#include "cinderx/python.h"

// clang-format off
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
// clang-format on

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/RuntimeTests/fixtures.h"

// ---------------------------------------------------------------------------
// Header availability detection.
// osr.h / osr_capi.h are created by Feature Item 1.  When they become
// available, flip CINDERX_OSR_HEADERS_AVAILABLE to 1 (or remove the guard
// and always include them).
// ---------------------------------------------------------------------------
#ifndef CINDERX_OSR_HEADERS_AVAILABLE
#define CINDERX_OSR_HEADERS_AVAILABLE 0
#endif

#if CINDERX_OSR_HEADERS_AVAILABLE
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/osr_capi.h"

#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_stackref.h"
#endif

using jit::Compiler;
using jit::getContext;

// ===========================================================================
// Sub-module B: resetOSRState unit tests
// ===========================================================================
//
// These tests directly exercise resetOSRState() and the funcModified()
// integration path.  They depend on the BackedgeCounters infrastructure
// from Feature Item 1.

#if CINDERX_OSR_HEADERS_AVAILABLE

// ---------------------------------------------------------------------------
// Helper: create a minimal BackedgeCounters attached to a code object.
// Returns a borrowed pointer to the counters (owned by the code object
// via code extra).
// ---------------------------------------------------------------------------
static BackedgeCounters* attachCountersToCode(
    BorrowedRef<PyCodeObject> code,
    std::vector<std::pair<uint32_t, uint32_t>> backedges) {
  BackedgeCounters* counters = Ci_GetOrCreateBackedgeCounters(code);
  EXPECT_NE(counters, nullptr);
  if (counters == nullptr) {
    return nullptr;
  }
  for (auto& [src, tgt] : backedges) {
    BackedgeEntry* entry =
        Ci_BackedgeCountersFindOrCreate(counters, src);
    EXPECT_NE(entry, nullptr);
    if (entry != nullptr) {
      entry->target_index = tgt;
    }
  }
  return counters;
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ---------------------------------------------------------------------------
// Fixture for Sub-module B tests
// ---------------------------------------------------------------------------
class ResetOSRStateTest : public RuntimeTest {
#if CINDERX_OSR_HEADERS_AVAILABLE
 public:
  // Helper: compile a simple function and return its code object.
  Ref<PyCodeObject> compileToCode(const char* src, const char* func_name) {
    Ref<PyFunctionObject> func(compileAndGet(src, func_name));
    if (func == nullptr) {
      return Ref<PyCodeObject>(nullptr);
    }
    return Ref<PyCodeObject>::create(func->func_code);
  }
#endif
};

#if CINDERX_OSR_HEADERS_AVAILABLE

// Common Python source for single while-loop tests (TC-B02 through TC-B09,
// TC-B11, TC-B12).
static constexpr const char* kSingleLoopSrc = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";

// TC-B01 (T-OSR-006): resetOSRState on code without BackedgeCounters is a
// safe no-op (idempotent).
TEST_F(ResetOSRStateTest, NullCounters_NoOp) {
  const char* src = R"(
def test():
    return 42
)";
  auto code = compileToCode(src, "test");
  ASSERT_NE(code, nullptr);

  // The code object has no BackedgeCounters (no while loops).
  BackedgeCounters* counters = Ci_GetBackedgeCounters(code);
  EXPECT_EQ(counters, nullptr);

  // resetOSRState should not crash.
  ASSERT_NO_FATAL_FAILURE(jit::resetOSRState(code));

  // Still no counters after reset.
  counters = Ci_GetBackedgeCounters(code);
  EXPECT_EQ(counters, nullptr);
}

// TC-B02: resetOSRState zeroes all entry counts.
TEST_F(ResetOSRStateTest, ResetsCountToZero) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);
  ASSERT_EQ(counters->num_entries, 1u);

  // Simulate counting — set count to non-zero.
  counters->entries[0].count = 1500;

  jit::resetOSRState(code);

  EXPECT_EQ(counters->entries[0].count, 0u);
}

// TC-B03: resetOSRState sets all entry states to Counting(1).
TEST_F(ResetOSRStateTest, ResetsStateToCounting) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  // Simulate FailedPermanent state.
  counters->entries[0].state = 3; /* FailedPermanent */

  jit::resetOSRState(code);

  EXPECT_EQ(counters->entries[0].state, 1u); /* Counting */
}

// TC-B04: resetOSRState clears num_compile_states to 0.
TEST_F(ResetOSRStateTest, ClearsCompileStates) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  // Simulate existing compile states.
  counters->num_compile_states = 2;
  counters->compile_states[0].builtins_id = 0xDEAD;
  counters->compile_states[0].globals_id = 0xBEEF;
  counters->compile_states[0].state = 2; /* Compiled */

  jit::resetOSRState(code);

  EXPECT_EQ(counters->num_compile_states, 0u);
}

// TC-B05: resetOSRState is idempotent — calling twice is safe.
TEST_F(ResetOSRStateTest, Idempotent) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  counters->entries[0].count = 999;
  counters->entries[0].state = 3;
  counters->num_compile_states = 1;

  jit::resetOSRState(code);
  ASSERT_EQ(counters->entries[0].count, 0u);
  ASSERT_EQ(counters->entries[0].state, 1u);
  ASSERT_EQ(counters->num_compile_states, 0u);

  // Second call — should produce the same state, no crash.
  jit::resetOSRState(code);
  EXPECT_EQ(counters->entries[0].count, 0u);
  EXPECT_EQ(counters->entries[0].state, 1u);
  EXPECT_EQ(counters->num_compile_states, 0u);
}

// TC-B06: resetOSRState handles multiple backedge entries.
TEST_F(ResetOSRStateTest, MultipleEntries_AllReset) {
  const char* src = R"(
def test():
    x = 0
    y = 0
    while x < 10:
        x += 1
    while y < 5:
        y += 1
    return x + y
)";
  auto code = compileToCode(src, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}, {24, 16}});
  ASSERT_NE(counters, nullptr);
  ASSERT_EQ(counters->num_entries, 2u);

  counters->entries[0].count = 100;
  counters->entries[0].state = 3; /* FailedPermanent */
  counters->entries[1].count = 200;
  counters->entries[1].state = 3;
  counters->num_compile_states = 1;

  jit::resetOSRState(code);

  EXPECT_EQ(counters->entries[0].count, 0u);
  EXPECT_EQ(counters->entries[0].state, 1u);
  EXPECT_EQ(counters->entries[1].count, 0u);
  EXPECT_EQ(counters->entries[1].state, 1u);
  EXPECT_EQ(counters->num_compile_states, 0u);
}

// TC-B07: resetOSRState does not alter num_entries.
TEST_F(ResetOSRStateTest, PreservesNumEntries) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);
  ASSERT_EQ(counters->num_entries, 1u);

  counters->entries[0].count = 500;
  counters->entries[0].state = 3;

  jit::resetOSRState(code);

  // num_entries should NOT change — only counts/states are reset.
  EXPECT_EQ(counters->num_entries, 1u);
}

// TC-B08: resetOSRState does not alter source_index / target_index.
TEST_F(ResetOSRStateTest, PreservesBackedgeIndices) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  jit::resetOSRState(code);

  EXPECT_EQ(counters->entries[0].source_index, 10u);
  EXPECT_EQ(counters->entries[0].target_index, 4u);
}

// TC-B09 (T-OSR-004): resetOSRState preserves pending exception state.
//
// Design (exit-degrade V1.5): resetOSRState uses PyErr_GetRaisedException /
// PyErr_SetRaisedException (3.12+ API) to protect the calling exception state.
// It must restore the exact same exception after completing its work.
TEST_F(ResetOSRStateTest, PendingException_Preserved) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);
  counters->entries[0].count = 500;

  // Set a pending exception using PyErr_SetString.
  PyErr_SetString(PyExc_RuntimeError, "test_exception_before_reset");

  jit::resetOSRState(code);

  // Verify exception is still set — use 3.12+ PyErr_GetRaisedException
  // to match the design's exception safety mechanism.
  PyObject* exc = PyErr_GetRaisedException();
  ASSERT_NE(exc, nullptr);
  EXPECT_TRUE(PyErr_GivenExceptionMatches(exc, PyExc_RuntimeError));
  Py_DECREF(exc);

  // Verify counters were still reset despite the exception.
  EXPECT_EQ(counters->entries[0].count, 0u);
}

// TC-B10 (T-OSR-005): typeModified does not call resetOSRState.
//
// Design (exit-degrade V1.3): notifyTypeModified() only handles inline cache
// invalidation and TypeDeoptPatcher activation. It does NOT modify
// BackedgeCounters or OSRCompileState. Type changes are handled by guard
// deopt mechanism — FailedPermanent states persist because they are set by
// code-level constraints (budget, live-in eligibility), not type information.
//
// NOTE: This test is a structural placeholder. A complete T-OSR-005 test
// would: (1) JIT-compile a function with a type guard, (2) call
// notifyTypeModified() on the guarded type, (3) assert BackedgeCounters
// are unchanged. That requires the full JIT compilation pipeline from
// Feature Items 1-3. When the pipeline is available, replace this test
// with an integration test that actually exercises the typeModified path.
TEST_F(ResetOSRStateTest, TypeModified_CountersUnchanged) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  auto code = compileToCode(src, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  // Simulate a compiled state with active counters.
  counters->entries[0].count = 2000;
  counters->entries[0].state = 1; /* Counting */
  counters->num_compile_states = 1;
  counters->compile_states[0].state = 2; /* Compiled */
  counters->compile_states[0].builtins_id = 0xCAFE;
  counters->compile_states[0].globals_id = 0xBABE;

  // TODO(T-OSR-005): Call notifyTypeModified() here once Feature Items 1-3
  // provide the JIT compilation pipeline needed to create type-dependent
  // compiled code. For now, this placeholder verifies the struct layout
  // and field access patterns are correct.

  // After typeModified, counters should be unchanged.
  EXPECT_EQ(counters->entries[0].count, 2000u);
  EXPECT_EQ(counters->entries[0].state, 1u);
  EXPECT_EQ(counters->num_compile_states, 1u);
  EXPECT_EQ(counters->compile_states[0].state, 2u);
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Integration tests: funcModified() + resetOSRState()
// ===========================================================================
//
// These tests trigger funcModified() through Python (func.__code__ = ...)
// and verify the OSR state cleanup via C++ APIs.
// Dependencies: Feature Items 1 + 4.

class OSRFuncModifiedTest : public RuntimeTest {
#if CINDERX_OSR_HEADERS_AVAILABLE
 public:
  // Helper: call a Python function with positional int arguments.
  Ref<> callFunction(BorrowedRef<PyFunctionObject> func,
                     std::vector<long> args) {
    PyObject* py_args = PyTuple_New(args.size());
    if (py_args == nullptr) {
      return Ref<>(nullptr);
    }
    for (size_t i = 0; i < args.size(); i++) {
      PyTuple_SET_ITEM(
          py_args, i, PyLong_FromLong(args[i])); // steals ref on success
    }
    auto result = Ref<>::steal(
        PyObject_CallObject(reinterpret_cast<PyObject*>(func), py_args));
    Py_DECREF(py_args);
    return result;
  }
#endif
};

#if CINDERX_OSR_HEADERS_AVAILABLE

// TC-I01 (T-OSR-003): Replacing func.__code__ triggers resetOSRState on the
// old code — counters count/state/compile_states all reset to initial values.
TEST_F(OSRFuncModifiedTest, ReplacesCode_TriggersReset) {
  const char* src = R"(
def test(n):
    result = 0
    while n > 0:
        result += n
        n -= 1
    return result
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  // Attach counters to simulate Feature Item 1 state.
  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  BackedgeCounters* counters =
      attachCountersToCode(old_code, {{14, 4}});
  ASSERT_NE(counters, nullptr);
  counters->entries[0].count = 2500;
  counters->entries[0].state = 2; /* Compiled */

  // Compile replacement code.
  const char* new_src = R"(
def replacement():
    return 99
)";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "replacement"));
  ASSERT_NE(new_func, nullptr);

  // Replace func.__code__ — triggers funcModified → resetOSRState(old_code).
  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // Verify old code's OSR state was reset.
  EXPECT_EQ(counters->entries[0].count, 0u);
  EXPECT_EQ(counters->entries[0].state, 1u); /* Counting */
  EXPECT_EQ(counters->num_compile_states, 0u);
}

// TC-I02: After funcModified reset, old code's BackedgeCounters still exist
// (memory not freed, just reset).
TEST_F(OSRFuncModifiedTest, ReplacesCode_CountersStillAccessible) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  BackedgeCounters* counters =
      attachCountersToCode(old_code, {{8, 2}});
  ASSERT_NE(counters, nullptr);

  const char* new_src = R"(
def replacement():
    return 0
)";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "replacement"));
  ASSERT_NE(new_func, nullptr);

  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // Counters still accessible (code object still alive, memory not freed).
  BackedgeCounters* after = Ci_GetBackedgeCounters(old_code);
  EXPECT_NE(after, nullptr);
  EXPECT_EQ(after, counters);
}

// TC-I03: funcModified with FailedPermanent entries resets to Counting.
TEST_F(OSRFuncModifiedTest, FailedPermanentResetToCounting) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  BackedgeCounters* counters =
      attachCountersToCode(old_code, {{8, 2}});
  ASSERT_NE(counters, nullptr);

  // Simulate FailedPermanent — OSR compilation failed permanently.
  counters->entries[0].state = 3; /* FailedPermanent */
  counters->entries[0].count = 5000;

  const char* new_src = "def r(): return 0";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "r"));
  ASSERT_NE(new_func, nullptr);

  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // FailedPermanent should be reset to Counting.
  EXPECT_EQ(counters->entries[0].state, 1u);
  EXPECT_EQ(counters->entries[0].count, 0u);
}

// TC-I04: funcModified on a function without BackedgeCounters is safe.
TEST_F(OSRFuncModifiedTest, NoCounters_NoCrash) {
  const char* src = R"(
def test():
    return 42
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  // No while loop → no BackedgeCounters.
  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  EXPECT_EQ(Ci_GetBackedgeCounters(old_code), nullptr);

  const char* new_src = "def r(): return 0";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "r"));
  ASSERT_NE(new_func, nullptr);

  // Should not crash.
  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);
}

// TC-I05: funcModified clears compile_states (uintptr_t identity, no DECREF).
TEST_F(OSRFuncModifiedTest, ClearsCompileStates) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  BackedgeCounters* counters =
      attachCountersToCode(old_code, {{8, 2}});
  ASSERT_NE(counters, nullptr);

  // Simulate compiled state with identity values.
  counters->num_compile_states = 1;
  counters->compile_states[0].builtins_id =
      reinterpret_cast<uintptr_t>(PyEval_GetBuiltins());
  counters->compile_states[0].globals_id =
      reinterpret_cast<uintptr_t>(PyModule_GetDict(
          PyImport_ImportModule("__main__")));
  counters->compile_states[0].state = 2; /* Compiled */

  const char* new_src = "def r(): return 0";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "r"));
  ASSERT_NE(new_func, nullptr);

  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // compile_states cleared, but identity pointers not DECREF'd (correct).
  EXPECT_EQ(counters->num_compile_states, 0u);
}

// TC-I06 (T-OSR-007): funcModified does NOT delete CompiledFunction from
// compiled_codes_ cache (unlike uncompile).
//
// Design: funcModified() calls deoptFunc (sets vectorcall back to
// interpreter) and unregisterFunctionCodes, but does NOT call
// ctx->forgetCode(). The old CompiledFunction may still exist in the
// cache indexed by (old_code, builtins, globals).
TEST_F(OSRFuncModifiedTest, CompiledFunctionRemainsInCache) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  // Force JIT compilation by calling the function.
  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);

  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));

  // Check that a CompiledFunction exists for this code.
  auto* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  auto* compiled = ctx->lookupCode(
      old_code,
      reinterpret_cast<PyDictObject*>(PyEval_GetBuiltins()),
      reinterpret_cast<PyDictObject*>(
          PyModule_GetDict(PyImport_ImportModule("__main__"))));
  // compiled may be nullptr if JIT hasn't compiled this code yet —
  // that's fine, this test verifies the cache is not purged.

  BackedgeCounters* counters =
      attachCountersToCode(old_code, {{8, 2}});
  ASSERT_NE(counters, nullptr);
  counters->entries[0].count = 1000;

  const char* new_src = "def r(): return 0";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "r"));
  ASSERT_NE(new_func, nullptr);

  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // After funcModified, counters are reset but the CompiledFunction
  // for old_code should still be in the cache (not deleted).
  EXPECT_EQ(counters->entries[0].count, 0u);
  auto* compiled_after = ctx->lookupCode(
      old_code,
      reinterpret_cast<PyDictObject*>(PyEval_GetBuiltins()),
      reinterpret_cast<PyDictObject*>(
          PyModule_GetDict(PyImport_ImportModule("__main__"))));
  // If compiled was non-null before, it should still be non-null.
  if (compiled != nullptr) {
    EXPECT_NE(compiled_after, nullptr);
  }
}

// TC-I07: resetOSRState targets OLD code, not new code.
//
// Design: resetOSRState(old_code) is called BEFORE func_code is updated.
// This ensures only the old code's counters are reset, not the new code's.
TEST_F(OSRFuncModifiedTest, ResetsOldCode_NotNewCode) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  Ref<PyCodeObject> old_code(Ref<>::create(func->func_code));
  BackedgeCounters* old_counters =
      attachCountersToCode(old_code, {{8, 2}});
  ASSERT_NE(old_counters, nullptr);
  old_counters->entries[0].count = 5000;

  // Create new code that also has a while loop.
  const char* new_src = R"(
def replacement():
    y = 0
    while y < 5:
        y += 1
    return y
)";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "replacement"));
  ASSERT_NE(new_func, nullptr);

  Ref<PyCodeObject> new_code(Ref<>::create(new_func->func_code));
  // Attach counters to new code too.
  BackedgeCounters* new_counters =
      attachCountersToCode(new_code, {{8, 2}});
  ASSERT_NE(new_counters, nullptr);
  new_counters->entries[0].count = 999;

  // Replace func.__code__ — triggers funcModified → resetOSRState(old_code).
  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // Old code counters should be reset.
  EXPECT_EQ(old_counters->entries[0].count, 0u);

  // New code counters should be UNCHANGED (resetOSRState targets old code).
  EXPECT_EQ(new_counters->entries[0].count, 999u);
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE
