// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/trigger_stats.h"

#include <atomic>

namespace jit {

namespace {

std::atomic<uint64_t> s_executable_alloc_calls{0};
std::atomic<uint64_t> s_executable_alloc_bytes{0};
std::atomic<uint64_t> s_compiled_function_creations{0};
std::atomic<uint64_t> s_machine_code_entries{0};

} // namespace

void triggerStatsOnExecutableAlloc(std::size_t bytes) {
  s_executable_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  s_executable_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void triggerStatsOnCompiledFunctionCreate() {
  s_compiled_function_creations.fetch_add(1, std::memory_order_relaxed);
}

TriggerStats triggerStatsSnapshot() {
  return TriggerStats{
      s_executable_alloc_calls.load(std::memory_order_relaxed),
      s_executable_alloc_bytes.load(std::memory_order_relaxed),
      s_compiled_function_creations.load(std::memory_order_relaxed),
      s_machine_code_entries.load(std::memory_order_relaxed),
  };
}

} // namespace jit
