// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/tls.h"

#include "cinderx/python.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/module_state.h"

namespace jit::codegen {

namespace {

constexpr size_t kMaxThreadStatePrologueInsns = 32;

} // namespace

std::optional<int32_t> parseThreadStatePrologue(
    const uint32_t* insns,
    size_t count) {
#if defined(CINDER_AARCH64)
  if (count == 0) {
    return std::nullopt;
  }

  size_t scan_start = 0;
  uint32_t reg = 0;
  bool matched = false;

  constexpr uint32_t kStpFpLrPreIndexMask = ~(0x7fU << 15);
  constexpr uint32_t kStpFpLrPreIndex = 0xa9807bfd;

  if (count >= 3 &&
      ((insns[0] & kStpFpLrPreIndexMask) == kStpFpLrPreIndex) && // stp x29, x30, [sp, #-N]!
      ((insns[1] & 0xffe0ffff) == 0x910003fd) && // mov x29, sp
      ((insns[2] & ~0x1f) == 0xd53bd040) // mrs x?, tpidr_el0
  ) {
    reg = insns[2] & 0x1f;
    scan_start = 3;
    matched = true;
  } else if ((insns[0] & ~0x1f) == 0xd53bd040) { // mrs x?, tpidr_el0
    reg = insns[0] & 0x1f;
    scan_start = 1;
    matched = true;
  }

  if (!matched) {
    return std::nullopt;
  }

  int32_t current_offset = 0;
  for (size_t index = scan_start; index < count; index++) {
    if (insns[index] == (0xf9400000 | (reg << 5))) {
      // ldr x0, [x?] - done
      return current_offset;
    }
    if ((insns[index] & ~0x7ffc00) == (0x91000000 | (reg << 5) | reg)) {
      // add x?, x?, #<imm>{, <shift>}
      uint32_t imm = (insns[index] >> 10) & 0xfff;
      if (insns[index] & (1 << 22)) {
        imm <<= 12;
      }
      current_offset += imm;
      continue;
    }
    return std::nullopt;
  }
  return std::nullopt;
#else
  (void)insns;
  (void)count;
  return std::nullopt;
#endif
}

void initThreadStateOffset() {
  auto module_state = cinderx::getModuleState();
  if (module_state->tstate_offset_inited) {
    return;
  }

  // The repetitive single byte checks here are ugly but they guarantee
  // that we're not reading unsafe memory. If we just tried to do a big
  // comparison we might encounter assembly that ends, but as long as
  // we keep seeing our pattern we know that the function is correct.
#if defined(CINDER_X86_64)
  uint8_t* ts_func = reinterpret_cast<uint8_t*>(&_PyThreadState_GetCurrent);

  if (ts_func[0] == 0x55 && // push rbp
      ts_func[1] == 0x48 && ts_func[2] == 0x89 &&
      ts_func[3] == 0xe5 && // mov rsp, rbp
      ts_func[4] == 0x64 && ts_func[5] == 0x48 && ts_func[6] == 0x8b &&
      ts_func[7] == 0x04 && ts_func[8] == 0x25) { // movq   %fs:OFFSET, %rax
    module_state->tstate_offset = *reinterpret_cast<int32_t*>(ts_func + 9);
  }
#elif defined(CINDER_AARCH64)
  uint32_t* ts_func = reinterpret_cast<uint32_t*>(&_PyThreadState_GetCurrent);
  module_state->tstate_offset =
      parseThreadStatePrologue(ts_func, kMaxThreadStatePrologueInsns)
          .value_or(-1);
#else
  CINDER_UNSUPPORTED
#endif

  module_state->tstate_offset_inited = true;
}
} // namespace jit::codegen
