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
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/osr_capi.h"
#include "cinderx/module_state.h"

#include <algorithm>
#include <cstring>

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

std::vector<BCIndex> collectBackedgeTargetOffsets(PyCodeObject* code) {
  std::vector<BCIndex> targets;
  for (auto instr : BytecodeInstructionBlock{code}) {
    if (instr.opcode() == JUMP_BACKWARD ||
        instr.opcode() == JUMP_BACKWARD_NO_INTERRUPT) {
      auto target = instr.getJumpTarget().asIndex();
      if (std::find(targets.begin(), targets.end(), target) == targets.end()) {
        targets.push_back(target);
      }
    }
  }
  return targets;
}

bool osrCompileBudgetCheck(PyCodeObject* code) {
  return countIndices(code) <= getConfig().osr_compile_budget_code_units;
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

int Ci_OSR_TryOSR(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    _Py_CODEUNIT* this_instr,
    uint32_t oparg,
    PyObject** out_result) {
  if (g_try_osr_hook.hook != nullptr) {
    int rc = g_try_osr_hook.hook(
        tstate, frame, this_instr, oparg, out_result, g_try_osr_hook.state);
    if (rc != 0) {
      popCurrentFrameForTestHook(tstate, frame);
    }
    return rc;
  }
  return 0;
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
