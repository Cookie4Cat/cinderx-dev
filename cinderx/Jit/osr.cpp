// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/osr.h"

#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#include "internal/pycore_frame.h"
#include "internal/pycore_interpframe.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_stackref.h"

#include "cinderx/Common/code.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/osr_capi.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/module_state.h"

#include <algorithm>
#include <cstring>
#include <vector>

int cinderx_osr_enabled = 0;
int cinderx_osr_capable = 0;
int cinderx_osr_state = 0;

namespace {

constexpr uint8_t kBackedgeCounting = CI_OSR_BACKEDGE_COUNTING;

using jit::BackedgeCounters;
using jit::BackedgeEntry;

struct TryOSRHookState {
  TryOSRHook hook{nullptr};
  void* state{nullptr};
};

TryOSRHookState g_try_osr_hook;

// Compile state constants (internal to Ci_OSR_TryOSR orchestrator).
constexpr uint8_t kCompileStateIdle = 0;
constexpr uint8_t kCompileStateCompiling = 1;
constexpr uint8_t kCompileStateCompiled = 2;
constexpr uint8_t kCompileStateFailedPermanent = 3;

Py_ssize_t osrBackedgeCountersExtraIndex() {
  auto* state = cinderx::getModuleState();
  if (state == nullptr) {
    return -1;
  }
  return state->osr_backedge_counters_extra_index;
}

BackedgeCounters* asBackedgeCounters(Ci_BackedgeCounters* counters) {
  return reinterpret_cast<BackedgeCounters*>(counters);
}

const BackedgeEntry* asBackedgeEntry(const Ci_BackedgeEntry* entry) {
  return reinterpret_cast<const BackedgeEntry*>(entry);
}

BackedgeEntry* asBackedgeEntry(Ci_BackedgeEntry* entry) {
  return reinterpret_cast<BackedgeEntry*>(entry);
}

Ci_BackedgeCounters* asCiBackedgeCounters(BackedgeCounters* counters) {
  return reinterpret_cast<Ci_BackedgeCounters*>(counters);
}

Ci_BackedgeEntry* asCiBackedgeEntry(BackedgeEntry* entry) {
  return reinterpret_cast<Ci_BackedgeEntry*>(entry);
}

void clearBackedgeCounters(BackedgeCounters* counters) {
  if (counters == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < counters->num_entries; i++) {
    counters->entries[i].count = 0;
    counters->entries[i].state = kBackedgeCounting;
  }
  counters->num_compile_states = 0;
  std::memset(
      counters->compile_states, 0, sizeof(counters->compile_states));
}

void popCurrentFrameForTestHook(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame) {
  if (tstate == nullptr || frame == nullptr || tstate->current_frame != frame ||
      frame->owner == FRAME_OWNED_BY_INTERPRETER) {
    return;
  }

  tstate->current_frame = frame->previous;
  jit::jitFrameClearExceptCode(frame);
  PyStackRef_CLEAR(frame->f_executable);
  Cix_PyThreadState_PopFrame(tstate, frame);
}

// ---------------------------------------------------------------------------
// Compile state helpers for Ci_OSR_TryOSR
// ---------------------------------------------------------------------------

jit::OSRCompileState* getOSRCompileState(
    BackedgeCounters* counters,
    uintptr_t builtins_id,
    uintptr_t globals_id) {
  for (uint32_t i = 0; i < counters->num_compile_states; i++) {
    if (counters->compile_states[i].builtins_id == builtins_id &&
        counters->compile_states[i].globals_id == globals_id) {
      return &counters->compile_states[i];
    }
  }
  return nullptr;
}

jit::OSRCompileState* getOrCreateOSRCompileState(
    BackedgeCounters* counters,
    uintptr_t builtins_id,
    uintptr_t globals_id) {
  jit::OSRCompileState* existing =
      getOSRCompileState(counters, builtins_id, globals_id);
  if (existing != nullptr) {
    return existing;
  }
  if (counters->num_compile_states >= CI_OSR_MAX_COMPILE_KEYS) {
    return nullptr;
  }
  uint32_t idx = counters->num_compile_states;
  counters->compile_states[idx].state = kCompileStateIdle;
  counters->compile_states[idx].builtins_id = builtins_id;
  counters->compile_states[idx].globals_id = globals_id;
  counters->num_compile_states++;
  return &counters->compile_states[idx];
}

bool isFailedPermanentPerCode(
    BackedgeCounters* counters,
    uint32_t source_idx) {
  for (uint32_t i = 0; i < counters->num_entries; i++) {
    if (counters->entries[i].source_index == source_idx) {
      return counters->entries[i].state == CI_OSR_BACKEDGE_FAILED_PERMANENT;
    }
  }
  return false;
}

void markFailedPermanentPerCode(
    BackedgeCounters* counters,
    uint32_t source_idx) {
  for (uint32_t i = 0; i < counters->num_entries; i++) {
    if (counters->entries[i].source_index == source_idx) {
      counters->entries[i].state = CI_OSR_BACKEDGE_FAILED_PERMANENT;
      return;
    }
  }
}

} // namespace

namespace jit {

void initOSRCodeExtraIndex() {
  auto* state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to initialize OSR code extra index without module state");
  JIT_CHECK(
      state->osr_backedge_counters_extra_index == -1,
      "Cannot re-initialize OSR code extra index");

  state->osr_backedge_counters_extra_index =
      PyUnstable_Eval_RequestCodeExtraIndex(PyMem_Free);
}

void finiOSRCodeExtraIndex() {
  auto* state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to finalize OSR code extra index without module state");
  JIT_CHECK(
      state->osr_backedge_counters_extra_index != -1,
      "Cannot finalize OSR code extra index before initialization");

  state->osr_backedge_counters_extra_index = -1;
}

BackedgeCounters* getBackedgeCounters(PyCodeObject* code) {
  if (code == nullptr) {
    return nullptr;
  }
  Py_ssize_t extra_index = osrBackedgeCountersExtraIndex();
  if (extra_index == -1) {
    return nullptr;
  }

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(
          reinterpret_cast<PyObject*>(code), extra_index, &data_ptr) < 0) {
    PyErr_Clear();
    return nullptr;
  }
  return reinterpret_cast<BackedgeCounters*>(data_ptr);
}

BackedgeCounters* getOrCreateBackedgeCounters(PyCodeObject* code) {
  if (code == nullptr) {
    return nullptr;
  }
  Py_ssize_t extra_index = osrBackedgeCountersExtraIndex();
  if (extra_index == -1) {
    return nullptr;
  }

  auto code_obj = reinterpret_cast<PyObject*>(code);
  CriticalSectionGuard guard(code_obj);

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(code_obj, extra_index, &data_ptr) < 0) {
    PyErr_Clear();
    return nullptr;
  }
  if (data_ptr != nullptr) {
    return reinterpret_cast<BackedgeCounters*>(data_ptr);
  }

  auto* counters = reinterpret_cast<BackedgeCounters*>(
      PyMem_Calloc(1, sizeof(BackedgeCounters)));
  if (counters == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  if (PyUnstable_Code_SetExtra(code_obj, extra_index, counters) < 0) {
    PyErr_Clear();
    PyMem_Free(counters);
    return nullptr;
  }

  return counters;
}

BackedgeEntry* findOrCreateBackedgeEntry(
    BackedgeCounters* counters,
    uint32_t source_index,
    uint32_t target_index) {
  if (counters == nullptr) {
    return nullptr;
  }

  for (uint32_t i = 0; i < counters->num_entries; i++) {
    auto& entry = counters->entries[i];
    if (entry.source_index == source_index) {
      return &entry;
    }
  }

  if (counters->num_entries >= CI_OSR_MAX_BACKEDGES) {
    return nullptr;
  }

  auto& entry = counters->entries[counters->num_entries++];
  entry.source_index = source_index;
  entry.target_index = target_index;
  entry.count = 0;
  entry.state = kBackedgeCounting;
  return &entry;
}

uint32_t getOSRBackedgeThreshold() {
  return getConfig().osr_backedge_threshold;
}

uint32_t computeJumpTargetIndex(
    PyCodeObject* code,
    uint32_t source_index,
    uint32_t oparg) {
  return source_index + 1 + inlineCacheSize(code, source_index) - oparg;
}

bool isOSREligible(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyCodeObject* code) {
  if (tstate == nullptr || frame == nullptr || code == nullptr) {
    return false;
  }

  if (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) {
    return false;
  }
  if (getConfig().frame_mode != FrameMode::kNormal) {
    return false;
  }
  if (frame->frame_obj != nullptr) {
    return false;
  }
  if (!PyStackRef_FunctionCheck(frame->f_funcobj)) {
    return false;
  }
  if (frame->stackpointer != _PyFrame_Stackbase(frame)) {
    return false;
  }
  return true;
}

void resetOSRState(PyCodeObject* code) {
  clearBackedgeCounters(getBackedgeCounters(code));
}

void syncOSRFlags() {
  const Config& config = getConfig();
  _Py_atomic_store_int_relaxed(
      &cinderx_osr_enabled, config.osr_enabled ? 1 : 0);
  _Py_atomic_store_int_relaxed(
      &cinderx_osr_capable, config.osr_capable ? 1 : 0);
  _Py_atomic_store_int_relaxed(
      &cinderx_osr_state, config.state == State::kRunning ? 1 : 0);
}

std::vector<BCOffset> collectBackedgeTargetOffsets(
    BorrowedRef<PyCodeObject> code) {
  std::vector<BCOffset> targets;
  for (auto instr : BytecodeInstructionBlock{code}) {
    if (instr.opcode() == JUMP_BACKWARD ||
        instr.opcode() == JUMP_BACKWARD_NO_INTERRUPT) {
      targets.emplace_back(instr.getJumpTarget());
    }
  }

  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  if (targets.size() > CI_OSR_MAX_BACKEDGES) {
    targets.resize(CI_OSR_MAX_BACKEDGES);
  }
  return targets;
}

bool osrCompileBudgetCheck(BorrowedRef<PyCodeObject> code) {
  return Py_SIZE(code) <= getConfig().osr_compile_budget_code_units;
}

const OSRMetadata* getOSREntry(
    BorrowedRef<CompiledFunction> compiled,
    BCOffset target_offset) {
  if (compiled == nullptr) {
    return nullptr;
  }
  const CodeRuntime* runtime = compiled->runtime();
  if (runtime == nullptr) {
    return nullptr;
  }
  for (const OSRMetadata& metadata : runtime->osrMetadatas()) {
    if (metadata.target_offset == target_offset) {
      return &metadata;
    }
  }
  return nullptr;
}

void* OSRMetadata::entryPoint(const CompiledFunction& cf) const {
  if (entry_point_offset < 0) {
    return nullptr;
  }
  auto code = cf.codeBuffer();
  auto offset = static_cast<size_t>(entry_point_offset);
  if (offset >= code.size()) {
    return nullptr;
  }
  return const_cast<std::byte*>(code.data()) + offset;
}

bool OSRMetadata::allReconstructible() const {
  return std::all_of(live_ins.begin(), live_ins.end(), [](const OSRLiveIn& li) {
    return li.reconstructible;
  });
}

// ---------------------------------------------------------------------------
// performOSR — core OSR entry implementation
// Feature Item 3: Frame State Migration
// SR-OSR-010, SR-OSR-013, SR-OSR-018
// ---------------------------------------------------------------------------

int performOSR(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    const OSRMetadata* osr_meta,
    const CompiledFunction* compiled,
    PyObject** out_result) {
  JIT_DCHECK(out_result != nullptr, "performOSR: out_result is null");
  JIT_DCHECK(
      tstate->current_frame == frame,
      "performOSR: frame != tstate->current_frame");

  // -- [0] Preflight checks (frame completely unchanged) --
  using OsrEntryFn = PyObject* (*)(OSRState*);
  OsrEntryFn entry_fn =
      reinterpret_cast<OsrEntryFn>(osr_meta->entryPoint(*compiled));
  if (entry_fn == nullptr) {
    return 0;
  }

  // MVP: empty operand stack check
  if (frame->stackpointer != _PyFrame_Stackbase(frame)) {
    return 0;
  }

  Py_ssize_t num_nlocalsplus = _PyFrame_GetCode(frame)->co_nlocalsplus;
  for (const auto& li : osr_meta->live_ins) {
    if (li.localsplus_index >= 0) {
      if (li.localsplus_index >= num_nlocalsplus) {
        return 0;
      }
      if (PyStackRef_IsNull(frame->localsplus[li.localsplus_index])) {
        return 0;
      }
    }
  }

  // -- [1] Pre-allocate all heap memory before modifying frame state --
  std::vector<bool> is_live_in_set;
  std::vector<PyObject*> deferred_decrefs;
  Py_ssize_t distinct_localsplus_live_in = 0;
  Py_ssize_t non_live_in_count = 0;

  try {
    is_live_in_set.resize(static_cast<std::size_t>(num_nlocalsplus), false);
    for (const auto& li : osr_meta->live_ins) {
      if (li.localsplus_index >= 0 && li.localsplus_index < num_nlocalsplus) {
        if (!is_live_in_set[li.localsplus_index]) {
          is_live_in_set[li.localsplus_index] = true;
          distinct_localsplus_live_in++;
        }
      }
    }

    non_live_in_count = num_nlocalsplus - distinct_localsplus_live_in;
    if (non_live_in_count > 0) {
      deferred_decrefs.resize(static_cast<std::size_t>(non_live_in_count));
    }
  } catch (const std::bad_alloc&) {
    return 0;
  }

  // From this point onward, the interpreter frame is committed to OSR: instr_ptr
  // is moved to the loop header and localsplus ownership is transferred before
  // entering the stub. The entry stub must not add recoverable failure paths
  // after this point unless this frame mutation is moved later or made rollbackable.
  // -- [2] Set frame->instr_ptr to loop header bytecode position --
  _Py_CODEUNIT* code_start = _PyCode_CODE(_PyFrame_GetCode(frame));
  frame->instr_ptr =
      code_start + osr_meta->target_offset.value() / sizeof(_Py_CODEUNIT);

  // -- [3] Collect non-live-in slots -> deferred_decrefs[] (steal) --
  Py_ssize_t n_deferred = 0;
  for (Py_ssize_t i = 0; i < num_nlocalsplus; i++) {
    if (is_live_in_set[i]) {
      continue;
    }
    _PyStackRef slot = frame->localsplus[i];
    if (!PyStackRef_IsNull(slot)) {
      deferred_decrefs[n_deferred++] = PyStackRef_AsPyObjectSteal(slot);
    }
    frame->localsplus[i] = PyStackRef_NULL;
  }

  // -- [4] Call OSR entry stub --
  OSRState state{tstate, frame, osr_meta};
  PyObject* result = entry_fn(&state);

  // -- [5] Execute deferred DECREFs --
  PyObject* saved_exc = nullptr;
  if (result == nullptr) {
    saved_exc = PyErr_GetRaisedException();
  }
  for (Py_ssize_t i = 0; i < n_deferred; i++) {
    Py_XDECREF(deferred_decrefs[i]);
  }
  if (saved_exc != nullptr) {
    PyErr_SetRaisedException(saved_exc);
  }

  // -- [6] Three-state return --
  if (result != nullptr) {
    *out_result = result;
    return 1;
  }
  return -1;
}

} // namespace jit

extern "C" {

Ci_BackedgeCounters* Ci_OSR_GetBackedgeCounters(PyCodeObject* code) {
  return asCiBackedgeCounters(jit::getBackedgeCounters(code));
}

Ci_BackedgeCounters* Ci_OSR_GetOrCreateBackedgeCounters(PyCodeObject* code) {
  return asCiBackedgeCounters(jit::getOrCreateBackedgeCounters(code));
}

Ci_BackedgeEntry* Ci_OSR_BackedgeCountersFindOrCreate(
    Ci_BackedgeCounters* counters,
    uint32_t source_idx,
    uint32_t target_idx) {
  return asCiBackedgeEntry(jit::findOrCreateBackedgeEntry(
      asBackedgeCounters(counters), source_idx, target_idx));
}

uint32_t Ci_OSR_BackedgeGetCount(const Ci_BackedgeEntry* entry) {
  return asBackedgeEntry(entry)->count;
}

uint8_t Ci_OSR_BackedgeGetState(const Ci_BackedgeEntry* entry) {
  return asBackedgeEntry(entry)->state;
}

void Ci_OSR_BackedgeSetCount(Ci_BackedgeEntry* entry, uint32_t count) {
  asBackedgeEntry(entry)->count = count;
}

void Ci_OSR_BackedgeSetState(Ci_BackedgeEntry* entry, uint8_t state) {
  asBackedgeEntry(entry)->state = state;
}

uint32_t Ci_OSR_BackedgeIncrement(Ci_BackedgeEntry* entry) {
  auto* backedge = asBackedgeEntry(entry);
  return ++backedge->count;
}

uint32_t Ci_OSR_ComputeJumpTargetIndex(
    PyCodeObject* code,
    uint32_t source_idx,
    uint32_t oparg) {
  return jit::computeJumpTargetIndex(code, source_idx, oparg);
}

bool Ci_OSR_IsEligible(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    PyCodeObject* code) {
  return jit::isOSREligible(tstate, frame, code);
}

// ---------------------------------------------------------------------------
// Ci_OSR_TryOSR — main OSR entry orchestrator
// Feature Item 3: Full implementation with compile state management
// SR-OSR-008
// ---------------------------------------------------------------------------

int Ci_OSR_TryOSR(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT* this_instr,
    uint32_t oparg,
    PyObject** out_result) {
  // Test hook: if installed, delegate and handle frame cleanup on non-zero rc
  if (g_try_osr_hook.hook != nullptr) {
    int rc = g_try_osr_hook.hook(
        tstate, frame, this_instr, oparg, out_result, g_try_osr_hook.state);
    if (rc != 0) {
      popCurrentFrameForTestHook(tstate, frame);
    }
    return rc;
  }

  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(
      PyStackRef_AsPyObjectBorrow(frame->f_funcobj));
  if (func == nullptr || !PyFunction_Check(func)) {
    return 0;
  }

  BorrowedRef<PyCodeObject> code = _PyFrame_GetCode(frame);
  _Py_CODEUNIT* code_start = _PyCode_CODE(code);
  JIT_DCHECK(
      this_instr >= code_start && this_instr < code_start + Py_SIZE(code),
      "Ci_OSR_TryOSR: instruction pointer outside current code object");
  uint32_t source_idx = static_cast<uint32_t>(this_instr - code_start);

  // Get or create backedge counters
  jit::BackedgeCounters* counters = jit::getBackedgeCounters(code);
  if (counters == nullptr) {
    counters = jit::getOrCreateBackedgeCounters(code);
    if (counters == nullptr) {
      return 0;
    }
  }

  // Check per-code permanent failure
  if (isFailedPermanentPerCode(counters, source_idx)) {
    return 0;
  }

  // Compute target index
  uint32_t target_idx =
      Ci_OSR_ComputeJumpTargetIndex(code, source_idx, oparg);

  // Per-CompilationKey state check
  uintptr_t builtins_id =
      reinterpret_cast<uintptr_t>(frame->f_builtins);
  uintptr_t globals_id =
      reinterpret_cast<uintptr_t>(frame->f_globals);

  jit::OSRCompileState* cs =
      getOSRCompileState(counters, builtins_id, globals_id);
  if (cs != nullptr) {
    if (cs->state == kCompileStateFailedPermanent) {
      return 0;
    }
    if (cs->state == kCompileStateCompiling) {
      return 0;
    }
    if (cs->state == kCompileStateCompiled) {
      goto cache_lookup;
    }
  }

  // Cache lookup / compilation path
cache_lookup:;
  jit::Context* ctx = jit::getContext();
  if (ctx == nullptr) {
    return 0;
  }

  BorrowedRef<PyDictObject> builtins =
      reinterpret_cast<PyDictObject*>(frame->f_builtins);
  BorrowedRef<PyDictObject> globals =
      reinterpret_cast<PyDictObject*>(frame->f_globals);

  BorrowedRef<jit::CompiledFunction> compiled =
      ctx->lookupCode(code, builtins, globals);
  if (compiled != nullptr) {
    const jit::OSRMetadata* osr =
        jit::getOSREntry(compiled, jit::BCIndex{target_idx}.asOffset());
    if (osr != nullptr) {
      return jit::performOSR(
          tstate, frame, osr, compiled.get(), out_result);
    }

    // Cache hit but no OSR entry for this backedge
    if (compiled->osrAware()) {
      // OSR-aware compilation explicitly skipped this backedge
      markFailedPermanentPerCode(counters, source_idx);
      return 0;
    }
    // Old cache without OSR entries — uncompile and recompile
    jit::uncompile(func);
  }

  // Allocate compile state slot
  cs = getOrCreateOSRCompileState(counters, builtins_id, globals_id);
  if (cs == nullptr) {
    markFailedPermanentPerCode(counters, source_idx);
    return 0;
  }

  // Budget check
  if (!jit::osrCompileBudgetCheck(code)) {
    markFailedPermanentPerCode(counters, source_idx);
    return 0;
  }

  // Synchronous compilation (holds GIL)
  cs->state = kCompileStateCompiling;
  jit::Result compile_result = jit::compileFunctionWithOSR(func);

  if (compile_result != jit::Result::OK) {
    cs->state = kCompileStateFailedPermanent;
    return 0;
  }

  cs->state = kCompileStateCompiled;

  // Compilation succeeded — lookup again and perform OSR
  compiled = ctx->lookupCode(code, builtins, globals);
  if (compiled == nullptr) {
    return 0;
  }

  const jit::OSRMetadata* osr =
      jit::getOSREntry(compiled, jit::BCIndex{target_idx}.asOffset());
  if (osr == nullptr) {
    return 0;
  }

  return jit::performOSR(tstate, frame, osr, compiled.get(), out_result);
}

void Ci_OSR_ResetState(PyCodeObject* code) {
  jit::resetOSRState(code);
}

void Ci_OSR_SetTestTryOSRHook(TryOSRHook hook, void* state) {
  g_try_osr_hook.hook = hook;
  g_try_osr_hook.state = state;
}

void Ci_OSR_ClearTestTryOSRHook(void) {
  g_try_osr_hook.hook = nullptr;
  g_try_osr_hook.state = nullptr;
}

void Ci_OSR_ClearBackedgeCountersForTesting(PyCodeObject* code) {
  Py_ssize_t extra_index = osrBackedgeCountersExtraIndex();
  if (code == nullptr || extra_index == -1) {
    return;
  }
  if (PyUnstable_Code_SetExtra(
          reinterpret_cast<PyObject*>(code), extra_index, nullptr) < 0) {
    PyErr_Clear();
  }
}

uint32_t Ci_OSR_GetBackedgeThreshold(void) {
  return jit::getOSRBackedgeThreshold();
}

} // extern "C"
