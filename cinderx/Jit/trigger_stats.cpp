// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/trigger_stats.h"

#include <atomic>

namespace jit {

namespace {

std::atomic<uint64_t> s_executable_alloc_calls{0};
std::atomic<uint64_t> s_executable_alloc_bytes{0};
std::atomic<uint64_t> s_compiled_function_creations{0};
std::atomic<uint64_t> s_machine_code_entries{0};
std::atomic<uint64_t> s_shadow_compile_success{0};
std::atomic<uint64_t> s_shadow_specialized_opcodes_consumed{0};
std::atomic<uint64_t> s_shadow_codegen_bytes{0};

} // namespace

void triggerStatsOnExecutableAlloc(std::size_t bytes) {
  s_executable_alloc_calls.fetch_add(1, std::memory_order_relaxed);
  s_executable_alloc_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void triggerStatsOnCompiledFunctionCreate() {
  s_compiled_function_creations.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnMachineCodeEntry() {
  s_machine_code_entries.fetch_add(1, std::memory_order_relaxed);
}

void triggerStatsOnShadowCompile(
    std::size_t code_bytes,
    uint64_t specialized_opcodes) {
  s_shadow_compile_success.fetch_add(1, std::memory_order_relaxed);
  s_shadow_specialized_opcodes_consumed.fetch_add(
      specialized_opcodes, std::memory_order_relaxed);
  s_shadow_codegen_bytes.fetch_add(code_bytes, std::memory_order_relaxed);
}

TriggerStats triggerStatsSnapshot() {
  return TriggerStats{
      s_executable_alloc_calls.load(std::memory_order_relaxed),
      s_executable_alloc_bytes.load(std::memory_order_relaxed),
      s_compiled_function_creations.load(std::memory_order_relaxed),
      s_machine_code_entries.load(std::memory_order_relaxed),
      s_shadow_compile_success.load(std::memory_order_relaxed),
      s_shadow_specialized_opcodes_consumed.load(std::memory_order_relaxed),
      s_shadow_codegen_bytes.load(std::memory_order_relaxed),
  };
}

} // namespace jit
