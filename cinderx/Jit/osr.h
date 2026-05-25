// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/hir/register.h"
#include "cinderx/Jit/pyjit_result.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"

#include <cstdint>
#include <vector>

namespace jit {

class CompiledFunction;

// ---------------------------------------------------------------------------
// Backedge counter data structures (Feature Item 1: Hot Loop Detection)
// ---------------------------------------------------------------------------

// Maximum number of backedges tracked per code object.
#define CI_OSR_MAX_BACKEDGES 16

// Maximum number of CompilationKey states per code object.
#define CI_OSR_MAX_COMPILE_KEYS 4

// Per-backedge entry: tracks a single JUMP_BACKWARD's count and state.
typedef struct BackedgeEntry {
  uint32_t source_index;   // Code-unit index of the backedge source instruction
  uint32_t target_index;   // Code-unit index of the loop header target
  uint32_t count;          // Backedge execution count
  uint8_t state;           // Hotness state: 1=Counting, 3=FailedPermanent
  uint8_t _pad[3];
} BackedgeEntry;

// Per-CompilationKey compile state, stored inside BackedgeCounters.
typedef struct OSRCompileState {
  // State machine:
  //   Idle(0) -> Compiling(1) -> Compiled(2)
  //   Compiling(1) -> [ALREADY_SCHEDULED] -> Idle(0)
  //   Compiling(1) -> [failure] -> FailedPermanent(3)
  uint8_t state;
  uintptr_t builtins_id;   // (uintptr_t)builtins — identity only
  uintptr_t globals_id;    // (uintptr_t)globals — identity only
} OSRCompileState;

// Backedge counter table, attached to PyCodeObject via code extra.
typedef struct BackedgeCounters {
  uint32_t num_entries;
  BackedgeEntry entries[CI_OSR_MAX_BACKEDGES];
  uint32_t num_compile_states;
  OSRCompileState compile_states[CI_OSR_MAX_COMPILE_KEYS];
} BackedgeCounters;

// ---------------------------------------------------------------------------
// OSR metadata data structures (Feature Item 2: OSR Compilation)
// Feature Item 3: OSR Entry (Frame State Migration) consumes these.
// ---------------------------------------------------------------------------

// Describes a single live-in value at an OSR entry point:
// where it comes from (localsplus or stack) and where it should go
// (JIT physical location after register allocation).
struct OSRLiveIn {
  int localsplus_index{-1};       // Source: f_localsplus[] index, -1 = not in localsplus
  int stack_index{-1};            // Source: operand stack index, -1 = not on stack
  codegen::PhyLocation destination; // Target: JIT physical location (filled after regalloc)
  hir::ValueKind value_kind{hir::ValueKind::kObject}; // Value type (MVP: kObject only)
  hir::RefKind ref_kind{hir::RefKind::kOwned}; // Reference type (MVP: kOwned only, see ADR-5)
  bool reconstructible{true};     // Can rebuild from interpreter frame
  bool is_phi{false};             // Whether this is an SSA phi node
  hir::Register* hir_reg{nullptr}; // Compilation-only source register
};

// Per-backedge OSR entry metadata. One instance per loop-header secondary
// entry point. Stored in CodeRuntime alongside DeoptMetadata.
struct OSRMetadata {
  BCOffset target_offset;                         // Loop header bytecode offset
  FrozenList<OSRLiveIn> live_ins;                  // Live-in mapping list
  int owned_ref_count{0};                          // kOwned live-in count
  ptrdiff_t entry_point_offset{-1};                // Stub code offset in code buffer

  // Environ VReg physical locations (set after regalloc).
  // Stub writes tstate/frame/func to these locations.
  codegen::PhyLocation tstate_location;
  codegen::PhyLocation func_location;
  codegen::PhyLocation frame_location;

  // Stub prologue frame layout parameters (must match kNormal JIT prologue).
  int32_t resume_frame_total_size{0};              // FrameInfo::size()
  int32_t resume_header_and_spill_size{0};         // FrameInfo::header_and_spill_size()
  codegen::PhyRegisterSet resume_saved_regs{0};    // env_.changed_regs & CALLEE_SAVE_REGS

  // Get the absolute entry point address from a CompiledFunction.
  // Returns nullptr if entry_point_offset < 0.
  void* entryPoint(const CompiledFunction& cf) const;

  // Returns true if all live-ins are reconstructible from the interpreter frame.
  bool allReconstructible() const;
};

// ---------------------------------------------------------------------------
// OSR entry state (Feature Item 3: OSR Entry)
// ---------------------------------------------------------------------------

// Data transfer structure between performOSR (C++) and OSR entry stub (asm).
// kNormal simplified version: only three pointers.
struct OSRState {
  PyThreadState* tstate;
  _PyInterpreterFrame* frame;
  const OSRMetadata* osr_meta;
};

// ---------------------------------------------------------------------------
// OSR C++ internal interfaces (Feature Items 1-3)
// ---------------------------------------------------------------------------

// Perform OSR entry: validate frame, collect non-live-in (deferred DECREF),
// call OSR entry stub, execute deferred DECREF, return three-state result.
//
// Returns:
//   1  = completed (JIT epilogue already PopFrame'd F)
//   0  = not attempted (frame unchanged)
//   -1 = exception (JIT epilogue already PopFrame'd F, PyErr set)
int performOSR(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    const OSRMetadata* osr_meta,
    const CompiledFunction* compiled,
    PyObject** out_result);

// Reset all OSR state for a code object (backedge counters + compile states).
// Called from funcModified() before func->func_code is updated.
// Idempotent: no-op if code has no BackedgeCounters.
void resetOSRState(PyCodeObject* code);

// Check if a frame is eligible for OSR at the current backedge.
// Rejection conditions: generator, free-threading, non-kNormal, non-empty
// operand stack, non-function frame, escaped frame.
bool isOSREligible(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyCodeObject* code);

// Compile a function with OSR entry points for all backedge targets.
// Uses standard preload() path + IsolatedPreloaders RAII protection.
// Returns OK on success, CANNOT_SPECIALIZE on OSR-specific rejection.
Result compileFunctionWithOSR(BorrowedRef<PyFunctionObject> func);

// Look up an OSR entry by loop-header target offset in a CompiledFunction.
// Returns nullptr if no matching entry exists.
const OSRMetadata* getOSREntry(
    BorrowedRef<CompiledFunction> compiled,
    BCOffset target_offset);

// Collect all JUMP_BACKWARD jump targets from a code object.
// Returns bytecode offsets for loop-header candidates.
std::vector<BCOffset> collectBackedgeTargetOffsets(
    BorrowedRef<PyCodeObject> code);

// Pre-compilation budget check based on static code features.
// Returns false if the function is too large/complex for OSR compilation.
bool osrCompileBudgetCheck(BorrowedRef<PyCodeObject> code);

// Sync OSR flags from jit::Config to C globals.
// Called during initialize(), enable_jit_impl(), pause, resume, finalize.
void syncOSRFlags();
void initOSRCodeExtraIndex();
void finiOSRCodeExtraIndex();

} // namespace jit
