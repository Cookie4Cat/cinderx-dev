// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <cstdlib>
#include <cstring>
#include <memory>

using jit::GenDataFooter;

class GenDataFooterTest : public RuntimeTest {};

TEST_F(GenDataFooterTest, FrameFooterPtrMatchesJitGeneratorObjectFooterPtr) {
#if PY_VERSION_HEX < 0x030E0000
  GTEST_SKIP() << "3.14+ generator frame layout only";
#else
  Ref<PyFunctionObject> func(compileAndGet(
      "async def coro(a, b):\n"
      "    return a + b\n",
      "coro"));
  ASSERT_NE(func.get(), nullptr);

  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  ASSERT_NE(code, nullptr);
  ASSERT_TRUE(code->co_flags & jit::kCoFlagsAnyGenerator);

  PyTypeObject* gen_type = cinderx::getModuleState()->gen_type;
  ASSERT_NE(gen_type, nullptr);

  const auto frame_slots =
      static_cast<std::size_t>(_PyFrame_NumSlotsForCodeObject(code));
  const auto expected_footer_offset =
      static_cast<std::size_t>(gen_type->tp_basicsize) +
      frame_slots * static_cast<std::size_t>(gen_type->tp_itemsize);
  const auto allocation_size = expected_footer_offset + sizeof(GenDataFooter*);
  const auto alignment = alignof(PyGenObject);
  const auto rounded_size = jit::roundUp(allocation_size, alignment);

  void* raw = std::aligned_alloc(alignment, rounded_size);
  ASSERT_NE(raw, nullptr);
  std::unique_ptr<void, decltype(&std::free)> storage(raw, std::free);
  std::memset(storage.get(), 0, rounded_size);

  auto* gen = reinterpret_cast<PyGenObject*>(storage.get());
  auto* frame = generatorFrame(gen);
  setFrameCode(frame, reinterpret_cast<PyObject*>(code));

  auto* by_gen = jit::jitGenDataFooterPtr(gen, code);
  auto* by_frame = jit::jitGenDataFooterPtr(frame);
  EXPECT_EQ(by_frame, by_gen);
  EXPECT_EQ(
      reinterpret_cast<char*>(by_frame),
      reinterpret_cast<char*>(gen) + expected_footer_offset);

  GenDataFooter footer;
  *by_frame = &footer;
  EXPECT_EQ(jit::jitGenDataFooter(frame), &footer);
  EXPECT_EQ(jit::jitFrameGetHeader(frame), &footer.frame_header);

  Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
#endif
}
