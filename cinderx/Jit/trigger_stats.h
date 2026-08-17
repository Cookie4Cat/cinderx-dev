// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>

namespace jit {

// Monotonic process-wide counters backing the trigger-proof report
// (ci_pipeline/jit311): tests assert not only on observable behaviour but on
// whether the machinery behind it actually fired.  On builds whose capability
// gate keeps the JIT unreachable these prove a negative -- executable memory
// was never allocated and no CompiledFunction ever existed -- and on
// execution-capable builds they prove the positive.
//
// Every increment site is a compilation-side cold path; per-call hot paths
// stay untouched (machine_code_entries is incremented by the CPython 3.11
// entry glue once that ships, and stays zero until then).
struct TriggerStats {
  // Number of executable-memory allocation calls made by the JIT's code
  // allocator, and the bytes they requested.
  uint64_t executable_alloc_calls;
  uint64_t executable_alloc_bytes;
  // Number of CompiledFunction objects ever created.
  uint64_t compiled_function_creations;
  // Number of times control entered JIT-compiled machine code.
  uint64_t machine_code_entries;
};

// Increment sites.  Relaxed atomics: the counters order nothing.  The
// machine_code_entries counter has no increment helper yet; it lands with
// the 3.11 entry glue and stays zero by construction until then.
void triggerStatsOnExecutableAlloc(std::size_t bytes);
void triggerStatsOnCompiledFunctionCreate();

// Read a consistent-enough snapshot for reporting.
TriggerStats triggerStatsSnapshot();

} // namespace jit
