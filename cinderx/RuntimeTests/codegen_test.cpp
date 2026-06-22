// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/tls.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <iterator>

using namespace jit::codegen;

namespace jit::codegen {

class CodegenTest : public RuntimeTest {};

TEST_F(CodegenTest, TestPhyRegisterSet) {
  auto set = PhyRegisterSet(2) | PhyRegisterSet(3) | PhyRegisterSet(5);

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 3);
  ASSERT_EQ(set.GetFirst(), 2);
  ASSERT_EQ(set.GetLast(), 5);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveFirst();

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 2);
  ASSERT_EQ(set.GetFirst(), 3);
  ASSERT_EQ(set.GetLast(), 5);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveLast();

  ASSERT_EQ(set.Empty(), false);
  ASSERT_EQ(set.count(), 1);
  ASSERT_EQ(set.GetFirst(), 3);
  ASSERT_EQ(set.GetLast(), 3);
  ASSERT_EQ(set.Has(3), true);

  set.RemoveFirst();

  ASSERT_EQ(set.Empty(), true);
  ASSERT_EQ(set.count(), 0);
  ASSERT_EQ(set.Has(3), false);
}

TEST_F(CodegenTest, DetectsThreadStateOffset) {
#if defined(CINDER_AARCH64)
  auto module_state = cinderx::getModuleState();
  bool old_inited = module_state->tstate_offset_inited;
  int32_t old_offset = module_state->tstate_offset;

  module_state->tstate_offset_inited = false;
  module_state->tstate_offset = -1;

  initThreadStateOffset();

  EXPECT_TRUE(module_state->tstate_offset_inited);
  EXPECT_NE(module_state->tstate_offset, -1);
  if (module_state->tstate_offset != -1) {
    uintptr_t tpidr;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    PyThreadState* expected = _PyThreadState_GetCurrent();
    auto actual = reinterpret_cast<PyThreadState*>(
        *reinterpret_cast<uintptr_t*>(
            tpidr + module_state->tstate_offset));
    EXPECT_EQ(actual, expected);
  }

  module_state->tstate_offset_inited = old_inited;
  module_state->tstate_offset = old_offset;
#else
  GTEST_SKIP() << "AArch64-specific thread-state offset detection";
#endif
}

TEST_F(CodegenTest, ParsesThreadStateStandardPrologue) {
#if defined(CINDER_AARCH64)
  const uint32_t code[] = {
      0xa9bf7bfd, // stp x29, x30, [sp, #-16]!
      0x910003fd, // mov x29, sp
      0xd53bd050, // mrs x16, tpidr_el0
      0x91404210, // add x16, x16, #0x10, lsl #12
      0x9107c210, // add x16, x16, #0x1f0
      0xf9400200, // ldr x0, [x16]
      0xd65f03c0, // ret
  };

  auto offset = parseThreadStatePrologue(code, std::size(code));
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, (0x10 << 12) + 0x1f0);
#else
  GTEST_SKIP() << "AArch64-specific thread-state prologue parser";
#endif
}

TEST_F(CodegenTest, ParsesThreadStateNoPrologueVariant) {
#if defined(CINDER_AARCH64)
  const uint32_t code[] = {
      0xd53bd050, // mrs x16, tpidr_el0
      0x91082210, // add x16, x16, #0x208
      0xf9400200, // ldr x0, [x16]
      0xd65f03c0, // ret
  };

  auto offset = parseThreadStatePrologue(code, std::size(code));
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset, 0x208);
#else
  GTEST_SKIP() << "AArch64-specific thread-state prologue parser";
#endif
}

TEST_F(CodegenTest, RejectsUnknownThreadStatePrologue) {
#if defined(CINDER_AARCH64)
  const uint32_t code[] = {
      0xd2800000, // mov x0, #0
      0xd65f03c0, // ret
  };

  EXPECT_FALSE(parseThreadStatePrologue(code, std::size(code)).has_value());
#else
  GTEST_SKIP() << "AArch64-specific thread-state prologue parser";
#endif
}

TEST_F(CodegenTest, RejectsUnterminatedThreadStatePrologue) {
#if defined(CINDER_AARCH64)
  const uint32_t code[] = {
      0xd53bd050, // mrs x16, tpidr_el0
      0x91082210, // add x16, x16, #0x208
      0x91004210, // add x16, x16, #0x10
  };

  EXPECT_FALSE(parseThreadStatePrologue(code, std::size(code)).has_value());
#else
  GTEST_SKIP() << "AArch64-specific thread-state prologue parser";
#endif
}

} // namespace jit::codegen
