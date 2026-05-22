// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "pycore_pyatomic_ft_wrappers.h"

#include <stdbool.h>
#include <stdint.h>

#if !defined(CINDER_AARCH64) && defined(__aarch64__)
#define CINDER_AARCH64
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// OSR C API — C/C++ compatible interface for interpreter.c consumption.
//
// interpreter.c compiles as C (CMakeLists.txt:332-346), cannot use C++
// namespaces. All OSR functions are exposed via extern "C" interfaces.
//
// Quick-path functions (Ci_OSR_IsEnabled) are static inline to avoid
// cross-C/C++ call overhead (~5-10ns PLT). They read C-visible global int
// flags, synced from jit::Config by syncOSRFlags() in osr.cpp.
// ---------------------------------------------------------------------------

// Global OSR flags (defined in osr.cpp, read by static inline helpers below).
// Synced from jit::Config during initialize/enable_jit/pause/resume/finalize.
extern int cinderx_osr_enabled;
extern int cinderx_osr_capable;
extern int cinderx_osr_state;

// Fast-path OSR enablement check. Called on every JUMP_BACKWARD_JIT backedge.
// ~1-2ns: one L1-cached atomic load + one conditional branch.
//
// All three conditions must hold:
//   1. osr_enabled — runtime toggle (Config::osr_enabled, -X osr-enabled)
//   2. osr_capable — set during jit::initialize() only (not runtime re-enable)
//   3. osr_state == 1 — JIT is in kRunning state
//
// Additionally gated by architecture and threading model:
//   - MVP: aarch64 only (CINDER_AARCH64)
//   - MVP: no free-threading support (Py_GIL_DISABLED)
static inline bool Ci_OSR_IsEnabled(void) {
  if (!_Py_atomic_load_int_relaxed(&cinderx_osr_enabled))
    return false;
  if (!_Py_atomic_load_int_relaxed(&cinderx_osr_capable))
    return false;
  if (_Py_atomic_load_int_relaxed(&cinderx_osr_state) != 1)
    return false;
#ifndef CINDER_AARCH64
  return false;   // MVP: aarch64 only
#endif
#ifdef Py_GIL_DISABLED
  return false;   // MVP: no free-threading
#endif
  return true;
}

// ---------------------------------------------------------------------------
// Opaque types for C consumption (C++ implementation in osr.cpp uses
// reinterpret_cast to convert between these and jit:: types).
// ---------------------------------------------------------------------------

typedef struct Ci_BackedgeCounters Ci_BackedgeCounters;
typedef struct Ci_BackedgeEntry Ci_BackedgeEntry;

// ---------------------------------------------------------------------------
// Backedge counter accessors (Feature Item 1: Hot Loop Detection)
// ---------------------------------------------------------------------------

// Maximum number of backedges tracked per code object.
#ifndef CI_OSR_MAX_BACKEDGES
#define CI_OSR_MAX_BACKEDGES 16
#endif

// Maximum number of CompilationKey states per code object.
#ifndef CI_OSR_MAX_COMPILE_KEYS
#define CI_OSR_MAX_COMPILE_KEYS 4
#endif

// Get the backedge counter table attached to a code object (may return NULL).
Ci_BackedgeCounters* Ci_OSR_GetBackedgeCounters(PyCodeObject* code);

// Get or create the backedge counter table for a code object.
// Returns NULL on allocation failure.
Ci_BackedgeCounters* Ci_OSR_GetOrCreateBackedgeCounters(PyCodeObject* code);

// Find or create a backedge entry for the given source instruction index.
// New entries are initialized with state=Counting(1), count=0.
// Returns NULL if the table is full (CI_OSR_MAX_BACKEDGES reached).
Ci_BackedgeEntry* Ci_OSR_BackedgeCountersFindOrCreate(
    Ci_BackedgeCounters* counters,
    uint32_t source_idx,
    uint32_t target_idx);

// BackedgeEntry state constants.
#define CI_OSR_BACKEDGE_COUNTING  1
#define CI_OSR_BACKEDGE_FAILED_PERMANENT  3

// BackedgeEntry accessors.
uint32_t Ci_OSR_BackedgeGetCount(const Ci_BackedgeEntry* entry);
uint8_t Ci_OSR_BackedgeGetState(const Ci_BackedgeEntry* entry);
void Ci_OSR_BackedgeSetCount(Ci_BackedgeEntry* entry, uint32_t count);
void Ci_OSR_BackedgeSetState(Ci_BackedgeEntry* entry, uint8_t state);

// Increment the backedge count and return the new value. MVP OSR is disabled
// under free-threading builds by Ci_OSR_IsEnabled(), so this is GIL-protected.
uint32_t Ci_OSR_BackedgeIncrement(Ci_BackedgeEntry* entry);

// Compute the loop-header target code-unit index from a backedge instruction.
// target = source_idx + 1 + inlineCacheSize(code, source_idx) - oparg
uint32_t Ci_OSR_ComputeJumpTargetIndex(
    PyCodeObject* code,
    uint32_t source_idx,
    uint32_t oparg);

// ---------------------------------------------------------------------------
// OSR eligibility and entry (Feature Items 1 + 3)
// ---------------------------------------------------------------------------

// Check if a frame is eligible for OSR at the current backedge point.
// Fast-path rejection for unsupported frame forms (generator, non-empty stack,
// non-function frame, escaped frame, free-threading, non-kNormal mode).
bool Ci_OSR_IsEligible(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyCodeObject* code);

// Attempt OSR: compile (if needed) and enter JIT code from the loop header.
//
// Called from the JUMP_BACKWARD_JIT bytecoded handler when backedge count
// reaches the threshold.
//
// Parameters:
//   tstate       - current thread state
//   frame        - current interpreter frame (tstate->current_frame)
//   this_instr   - pointer to the JUMP_BACKWARD_JIT instruction being executed
//   oparg        - the oparg of the backedge instruction (jump offset)
//   out_result   - [out] receives the PyObject* result on success (rc=1)
//
// Returns three-state value (see core contract in function design doc):
//   1  = completed — JIT epilogue already PopFrame'd F, result in *out_result
//   0  = not attempted — frame unchanged, continue interpreter execution
//   -1 = exception — JIT epilogue already PopFrame'd F, PyErr is set
//
// The bytecode handler is responsible for:
//   rc=1:  _Py_LeaveRecursiveCallPy(tstate); frame = tstate->current_frame;
//          LOAD_SP(); LOAD_IP(frame->return_offset); push(result); DISPATCH();
//   rc=-1: _Py_LeaveRecursiveCallPy(tstate); frame = tstate->current_frame;
//          (entry frame: return NULL; Python frame: goto error)
//   rc=0:  LOAD_SP(); fall through to osr_skip → DISPATCH
int Ci_OSR_TryOSR(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT* this_instr,
    uint32_t oparg,
    PyObject** out_result);

// Uppercase wrappers are used by cinder-bytecodes.c so the cases generator
// treats these as macros and does not inject its own stack spill/reload pairs.
// The OSR bytecode block manages SAVE_SP()/LOAD_SP() manually because
// Ci_OSR_TryOSR may consume the current frame before returning.
#define CI_OSR_IS_ELIGIBLE(tstate, frame, code) \
  Ci_OSR_IsEligible((tstate), (frame), (code))
#define CI_OSR_COMPUTE_JUMP_TARGET_INDEX(code, source_idx, oparg) \
  Ci_OSR_ComputeJumpTargetIndex((code), (source_idx), (oparg))
#define CI_OSR_GET_OR_CREATE_BACKEDGE_COUNTERS(code) \
  Ci_OSR_GetOrCreateBackedgeCounters((code))
#define CI_OSR_BACKEDGE_COUNTERS_FIND_OR_CREATE(counters, source_idx, target_idx) \
  Ci_OSR_BackedgeCountersFindOrCreate((counters), (source_idx), (target_idx))
#define CI_OSR_BACKEDGE_GET_STATE(entry) \
  Ci_OSR_BackedgeGetState((entry))
#define CI_OSR_BACKEDGE_INCREMENT(entry) \
  Ci_OSR_BackedgeIncrement((entry))
#define CI_OSR_GET_BACKEDGE_THRESHOLD() \
  Ci_OSR_GetBackedgeThreshold()
#define CI_OSR_BACKEDGE_SET_COUNT(entry, count) \
  Ci_OSR_BackedgeSetCount((entry), (count))
#define CI_OSR_BACKEDGE_SET_STATE(entry, state) \
  Ci_OSR_BackedgeSetState((entry), (state))
#define CI_OSR_TRY_OSR(tstate, frame, this_instr, oparg, out_result) \
  Ci_OSR_TryOSR((tstate), (frame), (this_instr), (oparg), (out_result))

// ---------------------------------------------------------------------------
// OSR state reset (Feature Item 4: OSR Exit and Degrade)
// ---------------------------------------------------------------------------

// Reset all OSR state for a code object. Called from funcModified() before
// func->func_code is updated. Resets backedge counters to Counting(1) with
// count=0, clears compile_states. Idempotent: no-op if no counters exist.
void Ci_OSR_ResetState(PyCodeObject* code);

// ---------------------------------------------------------------------------
// Test hooks (for RuntimeTests only, not called from production code)
// ---------------------------------------------------------------------------

// Hook type for TryOSR testing. Receives the same call context as
// Ci_OSR_TryOSR plus an opaque state pointer, and returns the same
// three-state value as Ci_OSR_TryOSR.
typedef int (*TryOSRHook)(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT* this_instr,
    uint32_t oparg,
    PyObject** out_result,
    void* state);

// Install/remove a test hook for Ci_OSR_TryOSR. When installed, the hook
// is called instead of the real TryOSR implementation.
void Ci_OSR_SetTestTryOSRHook(TryOSRHook hook, void* state);
void Ci_OSR_ClearTestTryOSRHook(void);

// Clear all backedge counters for a code object (testing utility).
void Ci_OSR_ClearBackedgeCountersForTesting(PyCodeObject* code);

// Get the current backedge threshold from Config.
uint32_t Ci_OSR_GetBackedgeThreshold(void);

#ifdef __cplusplus
} // extern "C"
#endif
