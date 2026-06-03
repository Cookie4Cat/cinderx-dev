// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <stdint.h>

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
} CodeExtra;

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

static inline uint32_t Ci_code_extra_load_skey_acquire(
    const CodeExtra* extra) {
  return __atomic_load_n(&extra->skey_word, __ATOMIC_ACQUIRE);
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  __atomic_store_n(&extra->skey_word, word, __ATOMIC_RELEASE);
}

#else

static inline void Ci_code_extra_incr_calls(CodeExtra* extra) {
  extra->calls += 1;
}

static inline uint64_t Ci_code_extra_get_calls(const CodeExtra* extra) {
  return extra->calls;
}

static inline uint32_t Ci_code_extra_load_skey_acquire(
    const CodeExtra* extra) {
  return extra->skey_word;
}

static inline void Ci_code_extra_store_skey_release(
    CodeExtra* extra,
    uint32_t word) {
  extra->skey_word = word;
}

#endif

#ifdef __cplusplus
}
#endif
