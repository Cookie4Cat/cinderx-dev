// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class PreloaderErrorPropagationTest : public RuntimeTest {
 public:
  PreloaderErrorPropagationTest()
      : RuntimeTest(static_cast<Flags>(kJit | kStaticCompiler)) {}
};

std::optional<int> constIndexForOpcode(
    BorrowedRef<PyCodeObject> code,
    int opcode) {
  for (const auto& instr : jit::BytecodeInstructionBlock{code}) {
    if (instr.opcode() == opcode) {
      return instr.oparg();
    }
  }
  return std::nullopt;
}

Ref<> cloneTuple(BorrowedRef<> tuple) {
  if (!PyTuple_Check(tuple)) {
    throw std::runtime_error{"expected a tuple descriptor"};
  }

  Py_ssize_t size = PyTuple_GET_SIZE(tuple.get());
  auto clone = Ref<>::steal(PyTuple_New(size));
  if (clone == nullptr) {
    throw std::runtime_error{"failed to clone tuple descriptor"};
  }

  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* item = PyTuple_GET_ITEM(tuple.get(), i);
    Py_INCREF(item);
    PyTuple_SET_ITEM(clone.get(), i, item);
  }
  return clone;
}

// Publish a fresh co_consts tuple after the Preloader has populated its
// identity-keyed maps, then restore the original tuple before the Preloader is
// destroyed. The guard owns the original tuple while it is detached, keeping
// the Preloader's borrowed map keys alive on every exit path.
class ScopedCodeConstsReplacement {
 public:
  ScopedCodeConstsReplacement(
      BorrowedRef<PyCodeObject> code,
      int const_index)
      : code_(code.get()), original_consts_(Ref<>::create(code->co_consts)) {
    Py_ssize_t size = PyTuple_GET_SIZE(code->co_consts);
    if (const_index < 0 || const_index >= size) {
      throw std::runtime_error{"constant index is out of range"};
    }

    auto replacement_consts = Ref<>::steal(PyTuple_New(size));
    if (replacement_consts == nullptr) {
      throw std::runtime_error{"failed to clone code constants"};
    }

    for (Py_ssize_t i = 0; i < size; i++) {
      PyObject* item = PyTuple_GET_ITEM(code->co_consts, i);
      if (i == const_index) {
        item = cloneTuple(item).release();
      } else {
        Py_INCREF(item);
      }
      PyTuple_SET_ITEM(replacement_consts.get(), i, item);
    }

    Py_SETREF(code_->co_consts, replacement_consts.release());
  }

  ~ScopedCodeConstsReplacement() {
    Py_SETREF(code_->co_consts, original_consts_.release());
  }

  ScopedCodeConstsReplacement(const ScopedCodeConstsReplacement&) = delete;
  ScopedCodeConstsReplacement& operator=(
      const ScopedCodeConstsReplacement&) = delete;

 private:
  PyCodeObject* code_;
  Ref<> original_consts_;
};

void expectContextualBuilderError(
    jit::hir::Preloader& preloader,
    std::string_view expected_error) {
  try {
    auto hir = jit::hir::buildHIR(preloader);
    FAIL() << "expected HIR builder failure, built " << hir->fullname;
  } catch (const std::runtime_error& error) {
    std::string message{error.what()};
    EXPECT_NE(message.find(expected_error), std::string::npos) << message;
    EXPECT_NE(message.find("jittestmodule:test"), std::string::npos) << message;
    EXPECT_NE(message.find("at offset"), std::string::npos) << message;
  }
}

TEST_F(
    PreloaderErrorPropagationTest,
    MissingPreloadedTypeThrowsContextualBuilderException) {
  const char* source = R"(
def test(value) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  auto preloader = jit::hir::Preloader::makePreloader(
      func, jit::makeFrameReifier(func->func_code));
  ASSERT_NE(preloader, nullptr);

  auto const_index = constIndexForOpcode(func->func_code, CAST);
  ASSERT_TRUE(const_index.has_value());
  ScopedCodeConstsReplacement replacement(func->func_code, *const_index);

  expectContextualBuilderError(
      *preloader, "CAST: Can't find type for type descr");
}

TEST_F(
    PreloaderErrorPropagationTest,
    MissingPreloadedFieldThrowsContextualBuilderException) {
  const char* source = R"(
class C:
    value: int

def test(instance: C):
    return instance.value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  auto preloader = jit::hir::Preloader::makePreloader(
      func, jit::makeFrameReifier(func->func_code));
  ASSERT_NE(preloader, nullptr);

  auto const_index = constIndexForOpcode(func->func_code, LOAD_FIELD);
  ASSERT_TRUE(const_index.has_value());
  ScopedCodeConstsReplacement replacement(func->func_code, *const_index);

  expectContextualBuilderError(
      *preloader, "LOAD_FIELD: Can't find field for descr");
}

} // namespace
