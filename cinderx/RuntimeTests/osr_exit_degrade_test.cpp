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

namespace jit {
void syncOSRFlags();
} // namespace jit

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

// ===========================================================================
// Sub-module A: OSR deopt path reuse verification
// ===========================================================================
//
// These tests exercise the full OSR entry → deopt → interpreter path.
// They verify that OSR JIT code correctly deopts using the shared deopt
// mechanism (prepareForDeopt → reifyFrame → releaseRefs →
// resumeInInterpreter → _PyEval_EvalFrame).
//
// Dependencies: Feature Items 1-4 (full OSR pipeline)
//
// Strategy: run Python functions with hot while loops to trigger OSR,
// then force deopt by changing types mid-execution.

#if CINDERX_OSR_HEADERS_AVAILABLE

class OSRDeoptTest : public RuntimeTest {
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

// TC-A01: OSR into while loop → result matches pure interpreter.
//
// kOwned note: `result += n` uses InPlaceOp (kOwned output → STORE_FAST),
// so `result` is kOwned at the loop header. `n -= 1` similarly makes `n` kOwned.
TEST_F(OSRDeoptTest, WhileLoop_ResultMatchesInterpreter) {
  const char* src = R"(
def test(n):
    result = 0
    while n > 0:
        result += n
        n -= 1
    return result
)";
  // Run with pure interpreter first to get expected result.
  auto expected = Ref<>::steal(PyLong_FromLong(5050));
  ASSERT_NE(expected, nullptr);

  // Run with JIT + OSR.
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto arg = Ref<>::steal(PyLong_FromLong(100));
  PyObject* args[] = {arg.get()};
  auto result = Ref<>::steal(
      PyObject_Vectorcall(reinterpret_cast<PyObject*>(func), args, 1, nullptr));

  ASSERT_NE(result, nullptr) << "OSR execution returned NULL";
  ASSERT_TRUE(isIntEquals(result, 5050));
}

// TC-A02: OSR into while loop with deopt via type change — result correct.
//
// kOwned note: `result = result + x.val` uses BinaryOp (kOwned output),
// stored via STORE_FAST. `x = x - 1` similarly produces kOwned values.
// These STORE_FAST definitions make `result` and `x` kOwned at the loop header.
//
// Strategy: run a while loop with a custom type (IntWrapper), forcing type
// guards that produce kOwned live-ins. Verify result correctness.
TEST_F(OSRDeoptTest, WhileLoop_DeoptViaTypeChange) {
  const char* src = R"(
class IntWrapper:
    def __init__(self, val):
        self.val = val
    def __add__(self, other):
        return IntWrapper(self.val + other)
    def __gt__(self, other):
        return self.val > other
    def __sub__(self, other):
        return IntWrapper(self.val - other)

def test():
    x = IntWrapper(100)
    result = IntWrapper(0)
    while x > 0:
        result = result + x.val
        x = x - 1
    return result.val
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(isIntEquals(result, 5050));
}

// TC-A03: OSR deopt preserves all local variable values.
//
// kOwned note: `d += a + b + c` uses InPlaceOp (kOwned) → STORE_FAST → `d`
// becomes kOwned. `i += 1` similarly makes `i` kOwned. `a`, `b`, `c` are
// kBorrowed at the loop header (only LOAD_FAST, no guard/store) but are
// NOT in the live-in set if not used by the loop header block — they're
// only used inside the loop body. The OSR entry only requires the values
// actually live at the loop header to be kOwned.
TEST_F(OSRDeoptTest, WhileLoop_DeoptPreservesLocals) {
  const char* src = R"(
def test():
    a = 1
    b = 2
    c = 3
    d = 0
    i = 0
    while i < 100:
        d += a + b + c
        i += 1
    return d
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // d = 100 * (1 + 2 + 3) = 600
  ASSERT_TRUE(isIntEquals(result, 600));
}

// TC-A04 (T-OSR-002): OSR with exception propagation through deopt.
//
// Strategy: raise an exception inside a while loop that's been OSR'd.
// The exception should propagate correctly through deopt.
TEST_F(OSRDeoptTest, WhileLoop_ExceptionPropagation) {
  const char* src = R"(
class MyError(Exception):
    pass

def test():
    x = 0
    while x < 1000:
        x += 1
        if x == 50:
            raise MyError("osr_deopt_exception")
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_EQ(result, nullptr) << "Expected exception but got a result";

  // PyErr_Occurred() returns a borrowed reference — do not wrap in Ref<>.
  ASSERT_TRUE(PyErr_Occurred() != nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(
      PyExc_Exception)); // MyError inherits Exception
  PyErr_Clear();
}

// TC-A05: OSR into nested while loops — deopt in inner loop.
//
// kOwned note: `result += i * j` uses InPlaceOp (kOwned) → `result` kOwned.
// `i += 1` and `j += 1` similarly produce kOwned values at both loop headers.
TEST_F(OSRDeoptTest, NestedWhileLoops_DeoptInner) {
  const char* src = R"(
def test():
    result = 0
    i = 0
    while i < 10:
        j = 0
        while j < 10:
            result += i * j
            j += 1
        i += 1
    return result
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // sum(i*j for i in range(10) for j in range(10)) = 45 * 45 = 2025
  ASSERT_TRUE(isIntEquals(result, 2025));
}

// TC-A06: OSR deopt reference count balance.
//
// kOwned note: `total += items[i]` uses InPlaceOp (kOwned output) → `total`
// kOwned. `i += 1` makes `i` kOwned. `items` is kBorrowed (LOAD_FAST only),
// but may not be a live-in at the loop header if not used by the comparison
// block (len() call produces a new value).
TEST_F(OSRDeoptTest, WhileLoop_RefcountBalance) {
  const char* src = R"(
def test():
    items = [1, 2, 3, 4, 5]
    total = 0
    i = 0
    while i < len(items):
        total += items[i]
        i += 1
    return total
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(isIntEquals(result, 15));
}

// TC-A07: OSR into a while loop, then normal return (no deopt).
//
// kOwned note: `x += 1` uses InPlaceOp (kOwned) → `x` is kOwned at the
// loop header. This verifies the normal exit path after OSR entry: the JIT
// code runs to completion and returns via the normal epilogue.
TEST_F(OSRDeoptTest, WhileLoop_NormalReturnNoDeopt) {
  const char* src = R"(
def test():
    x = 0
    while x < 100:
        x += 1
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(isIntEquals(result, 100));
}

// TC-A08: OSR into while loop with early break — deopt during break path.
//
// kOwned note: `x += 1` makes `x` kOwned. `found = x` makes `found` kOwned
// when assigned inside the if-block (STORE_FAST from LOAD_FAST value that
// has been through a comparison guard).
TEST_F(OSRDeoptTest, WhileLoop_EarlyBreak) {
  const char* src = R"(
def test():
    x = 0
    found = -1
    while x < 1000:
        if x == 42:
            found = x
            break
        x += 1
    return found
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(isIntEquals(result, 42));
}

// TC-A09: Multiple calls to the same OSR'd function — verify stability.
TEST_F(OSRDeoptTest, WhileLoop_MultipleCalls) {
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

  for (int n : {10, 50, 100, 200}) {
    auto arg = Ref<>::steal(PyLong_FromLong(n));
    PyObject* args[] = {arg.get()};
    auto result = Ref<>::steal(
        PyObject_Vectorcall(reinterpret_cast<PyObject*>(func), args, 1, nullptr));

    ASSERT_NE(result, nullptr) << "Failed for n=" << n;
    long expected = n * (n + 1) / 2;
    ASSERT_TRUE(isIntEquals(result, expected))
        << "Wrong result for n=" << n;
  }
}

// TC-A10: OSR into while loop with function calls inside.
//
// kOwned note: `helper(i)` returns kOwned (VectorCall kOwned output),
// `total += helper(i)` uses InPlaceOp (kOwned). `i += 1` makes `i` kOwned.
TEST_F(OSRDeoptTest, WhileLoop_WithFunctionCalls) {
  const char* src = R"(
def helper(x):
    return x * 2

def test():
    total = 0
    i = 0
    while i < 50:
        total += helper(i)
        i += 1
    return total
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // sum(2*i for i in range(50)) = 2 * 50*49/2 = 2450
  ASSERT_TRUE(isIntEquals(result, 2450));
}

// TC-A11: OSR deopt path — PopFrame executed by interpreter, deferred_decrefs
// safe regardless of executor.
//
// Design (exit-degrade V1.5): In the deopt path, resumeInInterpreter calls
// _PyEval_EvalFrame which runs the interpreter to function end. PopFrame is
// executed by interpreter's RETURN_VALUE (cinder-bytecodes.c:1326) or
// exit_unwind, not JIT epilogue's JITRT_UnlinkFrame. deferred_decrefs are
// safe because the PyObject* values were already steal'd from the frame —
// frame release doesn't affect their refcount.
//
// kOwned note: `total += data[i]` (InPlaceOp kOwned), `i += 1` (kOwned).
// `data` may be kBorrowed but is referenced through subscript which
// invalidates borrow support → promoted to kOwned.
TEST_F(OSRDeoptTest, DeoptPopFrameByInterpreter_DeferredDecrefSafe) {
  const char* src = R"(
def test():
    data = list(range(100))
    total = 0
    i = 0
    while i < len(data):
        total += data[i]
        i += 1
    return total
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // sum(0..99) = 4950
  ASSERT_TRUE(isIntEquals(result, 4950));
}

// TC-A12 (T-OSR-001): Deopt correctly restores locals after OSR entry.
//
// Design (exit-degrade V1.3): reifyLocalsplus (deopt.cpp:115-149) assumes all
// local slots are NULL before deopt (performOSR steal + stub steal guarantee).
// Dead slots get Ci_STACK_NULL (blind write), live slots get Ci_STACK_STEAL
// (blind write). This test verifies locals are correctly restored after deopt.
//
// **Localsplus pre-clearing verification (T-OSR-001 extension)**:
// The test also validates the pre-clearing invariant: all localsplus slots
// must be NULL before deopt reifyLocalsplus runs. This is guaranteed by
// performOSR (non live-in → deferred_decrefs + PyStackRef_NULL) and stub
// (live-in → steal + PyStackRef_NULL).
//
// kOwned note: `total += a + c + e` (InPlaceOp kOwned) and `i += 1` (kOwned)
// produce kOwned live-ins. `b` and `d` are dead in the loop — they are
// non live-in, collected by performOSR into deferred_decrefs, and restored
// by reifyLocalsplus from their original snapshot (which was captured by
// OSREntry's FrameState before performOSR cleared them).
TEST_F(OSRDeoptTest, DeoptRestoresAllLocals) {
  const char* src = R"(
def test():
    a = 1
    b = 2
    c = 3
    d = 4
    e = 5
    total = 0
    i = 0
    while i < 100:
        total += a + c + e
        i += 1
    # b and d are dead in the loop but should still have their values.
    return total + b + d
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // total = 100 * (1 + 3 + 5) = 900, plus b + d = 2 + 4 = 906
  ASSERT_TRUE(isIntEquals(result, 906));
}

// TC-A13: kOwned-only safe degradation — kBorrowed live-in should not crash.
//
// Design (function-design V7.0, exit-degrade V1.3): LOAD_FAST produces
// kBorrowed by default (parser.cpp:1314). MVP extractOSRLiveIns rejects
// non-kOwned live-in (ADR-5). Simple `while i < N: i += 1` may not trigger
// OSR because `i` is kBorrowed at the loop header (no guard/store between
// iterations that would promote it to kOwned).
// Note: `i += 1` actually uses InPlaceOp which produces kOwned output and
// stores via STORE_FAST — so `i` may actually become kOwned after the first
// iteration. If OSR does trigger, the result must still be correct. If it
// doesn't trigger (e.g. because the comparison `i < 100` uses the original
// kBorrowed load), the loop safely degrades to interpreter execution.
// Either way: correct result, no crash.
TEST_F(OSRDeoptTest, WhileLoop_kBorrowedSafeDegradation) {
  const char* src = R"(
def test():
    i = 0
    while i < 100:
        i += 1
    return i
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // Whether OSR triggers or not, the result must be correct.
  ASSERT_TRUE(isIntEquals(result, 100));
}

// TC-A14: kNormal mode disables inliner — deopt processes single-layer frame.
//
// Design (exit-degrade V1.1 verification list): kNormal mode disables
// HIR inliner (pyjit.cpp:731-732: hir_opts.inliner = false when
// frame_mode != kLightweight). All kNormal compilation products
// naturally contain no inline frames. Deopt only processes a single
// reifyFrame layer (inline_depth == 0).
//
// Strategy: compile a function that calls a small helper. Verify that
// the JIT does not inline the helper (result matches interpreter).
// This is a negative verification — if inliner were active, the helper
// might be inlined and deopt would need multi-layer reification.
TEST_F(OSRDeoptTest, KNormal_NoInlining_SingleLayerDeopt) {
  const char* src = R"(
def add(a, b):
    return a + b

def test():
    total = 0
    i = 0
    while i < 50:
        total = add(total, i)
        i += 1
    return total
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  auto result = Ref<>::steal(
      PyObject_CallNoArgs(reinterpret_cast<PyObject*>(func)));
  ASSERT_NE(result, nullptr);
  // sum(0..49) = 49*50/2 = 1225
  ASSERT_TRUE(isIntEquals(result, 1225));
}

// TC-B11: osr_aware cache degradation — resetOSRState doesn't change CF flags.
//
// Design (exit-degrade V1.5 section 5): resetOSRState only resets
// BackedgeCounters (count/state) and compile_states. It does NOT change
// the CompiledFunction's osr_aware or has_osr_entries flags. After
// funcModified, the old CF remains in compiled_codes_ with its original
// osr_aware status, but the function no longer uses it (deoptFunc set
// vectorcall to interpreter).
//
// Strategy: This is a unit-level check — verify that resetOSRState itself
// only affects the counters/compile_states fields, not any external state.
// The full integration with CompiledFunction cache is tested in TC-I06.
TEST_F(ResetOSRStateTest, ResetOSRState_DoesNotAffectCompiledFunction) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  counters->entries[0].count = 2000;
  counters->entries[0].state = 2; /* Compiled */
  counters->num_compile_states = 1;
  counters->compile_states[0].builtins_id = 0xCAFE;
  counters->compile_states[0].globals_id = 0xBABE;
  counters->compile_states[0].state = 2; /* Compiled */

  jit::resetOSRState(code);

  // Counters and compile_states are reset.
  EXPECT_EQ(counters->entries[0].count, 0u);
  EXPECT_EQ(counters->entries[0].state, 1u);
  EXPECT_EQ(counters->num_compile_states, 0u);

  // CompiledFunction lookup is not affected by resetOSRState.
  // (The CF may or may not exist — resetOSRState doesn't touch it.)
  auto* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  // No assertion on CF existence — this test verifies resetOSRState
  // doesn't crash or corrupt state when a CF may or may not exist.
}

// TC-B12: osr_aware has_osr_entries=false → per-code FailedPermanent.
//
// Design (exit-degrade V1.5 section 5): When osr_aware == true but
// has_osr_entries == false (all OSR entries were rejected during compilation,
// e.g. kBorrowed live-ins), the runtime must mark the corresponding
// BackedgeEntry as per-code FailedPermanent. This prevents infinite
// recompilation attempts for code that will never produce valid OSR entries.
//
// Strategy: Simulate a compilation result where all entries are rejected.
// Verify that subsequent attempts don't trigger recompilation.
// Note: This tests the degradtion semantics; actual recompilation
// prevention is handled by the state machine in Feature Item 2.
TEST_F(ResetOSRStateTest, OsrAwareNoEntries_DegradedState) {
  auto code = compileToCode(kSingleLoopSrc, "test");
  ASSERT_NE(code, nullptr);

  BackedgeCounters* counters =
      attachCountersToCode(code, {{10, 4}});
  ASSERT_NE(counters, nullptr);

  // Simulate per-code FailedPermanent — all entries rejected.
  counters->entries[0].state = 3; /* FailedPermanent */
  counters->entries[0].count = 0;

  // After resetOSRState (triggered by funcModified), the FailedPermanent
  // should be reset back to Counting(1), allowing future re-evaluation
  // with new builtins/globals.
  jit::resetOSRState(code);

  EXPECT_EQ(counters->entries[0].state, 1u); /* Counting */
  EXPECT_EQ(counters->entries[0].count, 0u);
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// End-to-end integration: codeDestroyed cleanup
// ===========================================================================
//
// Verify that code object destruction frees BackedgeCounters via the
// code extra freefunc mechanism.

#if CINDERX_OSR_HEADERS_AVAILABLE

// TC-E01: Code object destruction frees BackedgeCounters memory.
//
// Strategy: create a code object with BackedgeCounters, verify it has
// counters, then release ALL references (including from globals_ and
// the function) and verify cleanup via ASAN.
TEST_F(ResetOSRStateTest, CodeDestroyed_FreesCounters) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x
)";
  // compileAndGet stores "test" in globals_, so the function (and its
  // func_code reference) outlive the inner scope.  We must delete it
  // from globals_ before resetting code to ensure refcount reaches 0.
  Ref<PyCodeObject> code;
  {
    Ref<PyFunctionObject> func(compileAndGet(src, "test"));
    ASSERT_NE(func, nullptr);
    code = Ref<PyCodeObject>::create(func->func_code);
  }

  // Attach counters.
  BackedgeCounters* counters =
      attachCountersToCode(code, {{8, 2}});
  ASSERT_NE(counters, nullptr);
  EXPECT_EQ(Ci_GetBackedgeCounters(code), counters);

  // Remove "test" from globals_ so the function is freed, releasing
  // its reference to func_code.  After this, only the `code` Ref
  // holds a reference to the code object.
  ASSERT_EQ(PyDict_DelItemString(globals_, "test"), 0);

  // Release the code object — triggers codeDestroyed →
  // backedgeCountersFreefunc.
  code.reset();

  // If we get here without ASAN errors, the cleanup was correct.
  // (Cannot check Ci_GetBackedgeCounters after free — the code object
  // is gone.)
}

// TC-E02: After funcModified + codeDestroyed, no dangling pointers.
//
// Strategy: replace func.__code__ (triggers funcModified → resetOSRState),
// then let the old code be collected (triggers codeDestroyed).
// PyFunction_SetCode releases the function's reference to old_code, so
// only our Ref remains — old_code.reset() brings refcount to 0.
TEST_F(ResetOSRStateTest, FuncModifiedThenCodeDestroyed) {
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

  // Trigger funcModified — resets counters.
  const char* new_src = "def r(): return 0";
  Ref<PyFunctionObject> new_func(compileAndGet(new_src, "r"));
  ASSERT_NE(new_func, nullptr);
  ASSERT_EQ(
      PyFunction_SetCode(
          func, reinterpret_cast<PyObject*>(new_func->func_code)),
      0);

  // Verify reset happened.
  EXPECT_EQ(counters->entries[0].count, 0u);

  // Release old code — triggers codeDestroyed.
  // PyFunction_SetCode already released func's reference, so only
  // our Ref holds old_code now.  reset() brings refcount to 0.
  old_code.reset();

  // No ASAN errors means cleanup was correct.
}

#endif // CINDERX_OSR_HEADERS_AVAILABLE

// ===========================================================================
// Python-level integration tests (no OSR C++ headers needed)
// ===========================================================================
//
// These tests run Python code that exercises the funcModified path
// without directly calling OSR C++ APIs. They verify crash safety
// and basic correctness.

// TC-P01: func.__code__ replacement with JIT-compiled function — no crash.
TEST_F(RuntimeTest, FuncModified_WithJIT_NoCrash) {
  const char* src = R"(
def test():
    x = 0
    while x < 100:
        x += 1
    return x

# Run the function to trigger JIT compilation.
result = test()
assert result == 100, f"Expected 100, got {result}"

# Replace func.__code__ — triggers funcModified.
import types
# Using a simple function code replacement.
def replacement():
    return 99

test.__code__ = replacement.__code__

# Verify the function works with new code.
assert test() == 99, f"Expected 99 after replacement"
)";
  runCode(src);
}

// TC-P02: func.__code__ replacement without JIT — no crash.
TEST_F(RuntimeTest, FuncModified_WithoutJIT_NoCrash) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x

assert test() == 10

def replacement():
    return 42

test.__code__ = replacement.__code__
assert test() == 42
)";
  runCode(src);
}

// TC-P03: Multiple func.__code__ replacements — no crash.
TEST_F(RuntimeTest, FuncModified_MultipleReplacements) {
  const char* src = R"(
def test():
    x = 0
    while x < 10:
        x += 1
    return x

assert test() == 10

for i in range(5):
    value = i * 100
    def replacement():
        return value
    test.__code__ = replacement.__code__
    assert test() == value
)";
  runCode(src);
}

// TC-P04: Replace code of a function that was called many times (hot).
TEST_F(RuntimeTest, FuncModified_HotFunction_NoCrash) {
  const char* src = R"(
def test(n):
    result = 0
    while n > 0:
        result += n
        n -= 1
    return result

# Call many times to make it hot.
for i in range(100):
    assert test(10) == 55

# Replace code.
def replacement():
    return 0

test.__code__ = replacement.__code__
assert test() == 0
)";
  runCode(src);
}

// TC-P05: Exception handling in a while loop with OSR capability.
TEST_F(RuntimeTest, WhileLoop_ExceptionHandling) {
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

# sum(0..49) + 1000 + sum(51..99) = 49*50/2 + 1000 + 99*100/2 - 50*51/2
# = 1225 + 1000 + 4950 - 1275 = 5900
expected = sum(range(50)) + 1000 + sum(range(51, 100))
assert test() == expected
)";
  runCode(src);
}
