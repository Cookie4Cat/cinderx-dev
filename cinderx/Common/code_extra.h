// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <stdint.h>

// skey_word bit layout for cached AutoJIT structure keys:
//   bit 31: classification payload is valid.
//   bit 30: steady-state policy decided the code object is cold enough to stop
//           per-frame call counting.
//   bits 0-23: packed StructureKey payload. The gap between bit 30 and bit 23
//              is reserved for future flags so payload width can stay stable.
#define CI_CODE_EXTRA_SKEY_VALID_BIT 0x80000000u
#define CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT 0x40000000u
#define CI_CODE_EXTRA_SKEY_PAYLOAD_MASK 0x00FFFFFFu

// roi_ctl bit layout for dynamic negative-ROI backoff:
//   bit 31: code object is frozen for this process.
//   bit 30: a deopt thread is already performing uncompile/backoff work.
//   bits 24-27: completed backoff round count.
#define CI_CODE_EXTRA_ROI_FROZEN_BIT 0x80000000u
#define CI_CODE_EXTRA_ROI_PENDING_BIT 0x40000000u
#define CI_CODE_EXTRA_ROI_ROUND_SHIFT 24
#define CI_CODE_EXTRA_ROI_ROUND_MASK 0x0F000000u

#ifdef __cplusplus
extern "C" {
#endif

// Extra data attached to a code object.
typedef struct CodeExtra {
  union {
    // Number of times the code object has been called.
    uint64_t calls;
    // Used for unallocated free list code extras
    struct CodeExtra* next;
  };
  // Cached JIT-compiled entry for this code object. When jit_compiled is
  // non-NULL the code has been JIT-compiled with (jit_globals, jit_builtins),
  // letting a newly created PyFunctionObject with the same globals/builtins
  // skip the compiled_codes_ hashmap lookup in jit::Context. These are borrowed
  // pointers (jit::CompiledFunction* and PyObject*) owned by the JIT; the JIT
  // clears them before the CompiledFunction is freed (see context.cpp). Stored
  // as void* so the C interpreter can include this header. Accessed only by the
  // JIT under the free-threaded entrypoint guard; published/read with
  // release/acquire ordering (see context.cpp / pyjit.cpp).
  void* jit_compiled;
  void* jit_globals;
  void* jit_builtins;
  // Cached AutoJIT behavior classification. bit31 is the valid bit; the low
  // 24 bits are a StructureKey payload. Zero-initialized means unclassified.
  uint32_t skey_word;
  // Per-code dynamic negative-ROI backoff state. These fields are runtime
  // feedback and intentionally do not participate in StructureKey identity.
  uint32_t roi_deopt_count;
  uint32_t roi_ctl;
  uint64_t roi_recompile_floor;
  // Miscellaneous flags for code-level JIT bookkeeping.
  uint64_t flags;
  // 试用期计时判定（AutoJIT probation）：编译完成后的前 2K 次调用在
  // 入口包装器内按奇偶交替走解释/编译两条入口并计时，等样本均时对比
  // （带余量）决定转正或卸载冻结——时间维度直接回答"编译态是否劣于
  // 解释态"（计数阈值无法区分慢路径多但净更快的形态）。
  // probation_ctl：0=未启或已裁决，1=计时试用中（全局至多一个），
  // 2=排队等授予（静默按编译态执行，零全局触碰），3=已武装未报名
  // （编译完成但尚未被调用；首次调用时报名）。
  uint32_t probation_ctl;
  uint32_t probation_seq;
  uint64_t probation_interp_ns;
  uint64_t probation_jit_ns;
  // IC 压力密度（生产判据）：本 code 各内联 stub 慢路径进入计数，
  // 由 stub 慢尾直增（发射期烘焙地址），入口包装器按调用窗对比。
  uint64_t ic_slow_pressure;
  // 守卫自适应去特化（adaptive despec）：kGuardFailure 深度 deopt
  // 计数与粘滞态。despec_state：0=正常，1=已触发（去特化重编，永不
  // 回退）。单次观测型投机（特化形类型守卫）赌错时由此止损：越限即
  // 卸载并以去特化输入重编，多态受者的 deopt 风暴被一次重编封顶。
  uint32_t despec_deopt_count;
  uint32_t despec_state;
} CodeExtra;

#define CI_CODE_EXTRA_AUTO_JIT_DISABLED 1

// Thread-safe accessors for CodeExtra::calls.
// Under FT-Python, these use atomics to avoid data races.
#ifdef Py_GIL_DISABLED

// Note: _Py_atomic_add_uint64 uses seq_cst ordering, which might be stronger
// than needed for the calls counter. On x86-64, this is the same cost as
// relaxed (both emit lock xaddq). On ARM, a relaxed variant would be cheaper
// but there is no _Py_atomic_add_uint64_relaxed.
static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  _Py_atomic_add_uint64(&extra->calls, 1);
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return _Py_atomic_load_uint64_relaxed(&extra->calls);
}

static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* extra) {
  return __atomic_load_n(&extra->skey_word, __ATOMIC_ACQUIRE);
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_store_n(&extra->skey_word, word, __ATOMIC_RELEASE);
}

static inline void Ci_code_extra_or_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_fetch_or(&extra->skey_word, word, __ATOMIC_RELEASE);
}

static inline uint32_t Ci_code_extra_load_roi_ctl_relaxed(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->roi_ctl, __ATOMIC_RELAXED);
}

static inline void Ci_code_extra_store_roi_ctl_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_store_n(&extra->roi_ctl, word, __ATOMIC_RELEASE);
}

static inline int Ci_code_extra_cas_roi_ctl_release(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  return __atomic_compare_exchange_n(
      &extra->roi_ctl,
      expected,
      desired,
      0,
      __ATOMIC_RELEASE,
      __ATOMIC_RELAXED);
}

static inline uint32_t Ci_code_extra_incr_roi_deopt_count(CodeExtra* extra) {
  uint32_t old =
      __atomic_fetch_add(&extra->roi_deopt_count, 1, __ATOMIC_RELAXED);
  return old == UINT32_MAX ? UINT32_MAX : old + 1;
}

static inline uint32_t Ci_code_extra_load_despec_relaxed(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->despec_state, __ATOMIC_RELAXED);
}

static inline uint32_t Ci_code_extra_incr_despec_count(CodeExtra* extra) {
  uint32_t old =
      __atomic_fetch_add(&extra->despec_deopt_count, 1, __ATOMIC_RELAXED);
  return old == UINT32_MAX ? UINT32_MAX : old + 1;
}

static inline int Ci_code_extra_cas_despec(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  return __atomic_compare_exchange_n(
      &extra->despec_state,
      expected,
      desired,
      0,
      __ATOMIC_RELEASE,
      __ATOMIC_RELAXED);
}

static inline void Ci_code_extra_store_roi_deopt_count_relaxed(
    CodeExtra* extra,
    uint32_t value) {
  __atomic_store_n(&extra->roi_deopt_count, value, __ATOMIC_RELAXED);
}

static inline uint64_t Ci_code_extra_load_roi_recompile_floor_relaxed(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->roi_recompile_floor, __ATOMIC_RELAXED);
}

static inline void Ci_code_extra_store_roi_recompile_floor_release(
    CodeExtra* extra,
    uint64_t floor) {
  __atomic_store_n(&extra->roi_recompile_floor, floor, __ATOMIC_RELEASE);
}

static inline int Ci_code_extra_auto_jit_disabled(const CodeExtra* extra) {
  return (
      _Py_atomic_load_uint64_relaxed(&extra->flags) &
      CI_CODE_EXTRA_AUTO_JIT_DISABLED) != 0;
}

static inline void Ci_code_extra_disable_auto_jit(CodeExtra* extra) {
  uint64_t flags = _Py_atomic_load_uint64_relaxed(&extra->flags);
  _Py_atomic_store_uint64_relaxed(
      &extra->flags, flags | CI_CODE_EXTRA_AUTO_JIT_DISABLED);
}

#else

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  extra->calls += 1;
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return extra->calls;
}

static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* extra) {
  return extra->skey_word;
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->skey_word = word;
}

static inline void Ci_code_extra_or_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->skey_word |= word;
}

static inline uint32_t Ci_code_extra_load_roi_ctl_relaxed(
    const CodeExtra* extra) {
  return extra->roi_ctl;
}

static inline void Ci_code_extra_store_roi_ctl_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->roi_ctl = word;
}

static inline int Ci_code_extra_cas_roi_ctl_release(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  if (extra->roi_ctl != *expected) {
    *expected = extra->roi_ctl;
    return 0;
  }
  extra->roi_ctl = desired;
  return 1;
}

static inline uint32_t Ci_code_extra_incr_roi_deopt_count(CodeExtra* extra) {
  if (extra->roi_deopt_count != UINT32_MAX) {
    extra->roi_deopt_count += 1;
  }
  return extra->roi_deopt_count;
}

static inline uint32_t Ci_code_extra_load_despec_relaxed(
    const CodeExtra* extra) {
  return extra->despec_state;
}

static inline uint32_t Ci_code_extra_incr_despec_count(CodeExtra* extra) {
  if (extra->despec_deopt_count != UINT32_MAX) {
    extra->despec_deopt_count += 1;
  }
  return extra->despec_deopt_count;
}

static inline int Ci_code_extra_cas_despec(
    CodeExtra* extra,
    uint32_t* expected,
    uint32_t desired) {
  if (extra->despec_state != *expected) {
    *expected = extra->despec_state;
    return 0;
  }
  extra->despec_state = desired;
  return 1;
}

static inline void Ci_code_extra_store_roi_deopt_count_relaxed(
    CodeExtra* extra,
    uint32_t value) {
  extra->roi_deopt_count = value;
}

static inline uint64_t Ci_code_extra_load_roi_recompile_floor_relaxed(
    const CodeExtra* extra) {
  return extra->roi_recompile_floor;
}

static inline void Ci_code_extra_store_roi_recompile_floor_release(
    CodeExtra* extra,
    uint64_t floor) {
  extra->roi_recompile_floor = floor;
}

static inline int Ci_code_extra_auto_jit_disabled(const CodeExtra* extra) {
  return (extra->flags & CI_CODE_EXTRA_AUTO_JIT_DISABLED) != 0;
}

static inline void Ci_code_extra_disable_auto_jit(CodeExtra* extra) {
  extra->flags |= CI_CODE_EXTRA_AUTO_JIT_DISABLED;
}

#if PY_VERSION_HEX < 0x030C0000
/* 3.11 的 co_extra 数组布局私藏于 codeobject.c（头文件刻意不导出，
 * _PyCode_GetExtra 是出线调用）。按 vendored 3.11.6 逐字镜像做只读
 * 直读——与内联 stub 镜像 dict 预头布局同一论证：版本锚定、只读、
 * 写入仍走 PyUnstable_Code_SetExtra 正门。热路径消费者：入口分派的
 * 编译入口缓存（context.cpp）、[P3] 帧压栈计数的已决快速返回。 */
typedef struct {
  Py_ssize_t ce_size;
  void* ce_extras[1];
} CiCodeObjectExtra311;

static inline CodeExtra* Ci_code_extra_fast_read_311(
    PyCodeObject* code,
    Py_ssize_t index) {
  CiCodeObjectExtra311* co_extra = (CiCodeObjectExtra311*)code->co_extra;
  if (index < 0 || co_extra == NULL || index >= co_extra->ce_size) {
    return NULL;
  }
  return (CodeExtra*)co_extra->ce_extras[index];
}
#endif

#endif

#ifdef __cplusplus
}
#endif
