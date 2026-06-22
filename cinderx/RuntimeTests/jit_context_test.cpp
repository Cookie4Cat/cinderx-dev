// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

class JITContextTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITContextTest, UnwatchableBuiltins) {
  // This is a C++ test rather than in test_cinderjit so we can guarantee a
  // fresh runtime state with a watchable builtins dict when the test begins.
  const char* py_src = R"(
import builtins

def del_foo():
    global foo
    del foo

def func():
    foo
    builtins.__dict__[42] = 42
    del_foo()

foo = "hello"
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_EQ(result, Py_None);
}

class JITConfigTest : public RuntimeTest {};


TEST_F(JITConfigTest, IsJitUsableAfterInit) {
  EXPECT_TRUE(jit::isJitUsable());
}

TEST_F(JITConfigTest, IsJitInitialized) {
  EXPECT_TRUE(jit::isJitInitialized());
}

TEST_F(JITConfigTest, GlobalModuleStateAccessorMatchesModuleObjectState) {
  Ref<> mod = Ref<>::steal(PyImport_ImportModule("_cinderx"));
  ASSERT_NE(mod, nullptr);

  EXPECT_EQ(cinderx::getModuleState(), cinderx::getModuleState(mod));
}

TEST_F(JITConfigTest, IsJitNotPaused) {
  EXPECT_FALSE(jit::isJitPaused());
}

TEST_F(JITConfigTest, GetMutableConfigModifiesState) {
  auto& cfg = jit::getMutableConfig();
  auto orig_attr_cache_size = cfg.attr_cache_size;
  cfg.attr_cache_size = 8;
  EXPECT_EQ(jit::getConfig().attr_cache_size, 8);
  cfg.attr_cache_size = orig_attr_cache_size;
}

TEST_F(JITConfigTest, DefaultFrameMode) {
  auto mode = jit::getConfig().frame_mode;
  EXPECT_TRUE(
      mode == jit::FrameMode::kNormal || mode == jit::FrameMode::kLightweight);
}

TEST_F(JITConfigTest, DefaultAttrCachesEnabled) {
  bool attr_caches = jit::getConfig().attr_caches;
  EXPECT_TRUE(attr_caches || !attr_caches);
}

TEST_F(JITConfigTest, DefaultSpecializedOpcodes) {
  EXPECT_TRUE(jit::getConfig().specialized_opcodes);
}

TEST_F(JITConfigTest, DefaultStableFrame) {
  EXPECT_TRUE(jit::getConfig().stable_frame);
}

TEST_F(JITConfigTest, DefaultInlinerCostLimit) {
  EXPECT_GT(jit::getConfig().inliner_cost_limit, 0u);
}

TEST_F(JITConfigTest, DefaultSimplifierIterationLimit) {
  EXPECT_GT(jit::getConfig().simplifier.iteration_limit, 0u);
}

TEST_F(JITConfigTest, DefaultHIROptsEnabled) {
  const auto& opts = jit::getConfig().hir_opts;
  EXPECT_TRUE(opts.simplify);
  EXPECT_TRUE(opts.clean_cfg);
  EXPECT_TRUE(opts.dead_code_elim);
  EXPECT_TRUE(opts.list_prefix_reverse_assign);
  EXPECT_TRUE(opts.phi_elim);
}

TEST_F(JITConfigTest, DefaultLIROptsEnabled) {
  EXPECT_TRUE(jit::getConfig().lir_opts.inliner);
}

TEST_F(JITConfigTest, LogOptionsDefaults) {
  const auto& log = jit::getConfig().log;
  EXPECT_FALSE(log.debug);
  EXPECT_FALSE(log.dump_hir_initial);
  EXPECT_FALSE(log.dump_hir_passes);
  EXPECT_FALSE(log.dump_hir_final);
  EXPECT_FALSE(log.dump_lir);
  EXPECT_FALSE(log.dump_asm);
  EXPECT_EQ(log.output_file, stderr);
}

TEST_F(JITConfigTest, JitListOptionsDefaults) {
  const auto& jl = jit::getConfig().jit_list;
  EXPECT_TRUE(jl.filename.empty());
  EXPECT_FALSE(jl.error_on_parse);
  EXPECT_FALSE(jl.match_line_numbers);
}

TEST_F(JITConfigTest, GdbOptionsDefaults) {
  const auto& gdb = jit::getConfig().gdb;
  EXPECT_FALSE(gdb.supported);
  EXPECT_FALSE(gdb.write_elf_objects);
}


class JITPyjitTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITPyjitTest, IsJitUsable) {
  bool usable = jit::isJitUsable();
  EXPECT_TRUE(usable);
}

TEST_F(JITPyjitTest, CompileAndCheckVectorcall) {
  const char* py_src = R"(
def func(a: int) -> int:
    return a + 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  EXPECT_FALSE(compiled->codeBuffer().empty());
  EXPECT_NE(compiled->runtime(), nullptr);
}

TEST_F(JITPyjitTest, CompileTwoFunctions) {
  const char* py_src = R"(
def add(a: int, b: int) -> int:
    return a + b

def mul(a: int, b: int) -> int:
    return a * b
)";

  Ref<PyFunctionObject> add_func(compileAndGet(py_src, "add"));
  Ref<PyFunctionObject> mul_func(compileAndGet(py_src, "mul"));
  ASSERT_NE(add_func, nullptr);
  ASSERT_NE(mul_func, nullptr);

  {
    std::unique_ptr<jit::hir::Preloader> preloader(
        jit::hir::Preloader::makePreloader(
            add_func, jit::makeFrameReifier(add_func->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, add_func);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  {
    std::unique_ptr<jit::hir::Preloader> preloader(
        jit::hir::Preloader::makePreloader(
            mul_func, jit::makeFrameReifier(mul_func->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, mul_func);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  EXPECT_TRUE(jit_ctx_->didCompile(add_func));
  EXPECT_TRUE(jit_ctx_->didCompile(mul_func));
}

TEST_F(JITPyjitTest, CompiledFunctionAddrNonNull) {
  const char* py_src = R"(
def func() -> int:
    return 100
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  std::span<const std::byte> code_buf = compiled->codeBuffer();
  EXPECT_FALSE(code_buf.empty());
}

TEST_F(JITPyjitTest, ForgetCodeAndRecompile) {
  const char* py_src = R"(
def func() -> int:
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit_ctx_->forgetCode(func);

  compiled = jit_ctx_->lookupFunc(func);
  EXPECT_EQ(compiled, nullptr);
}

TEST_F(JITPyjitTest, AddRemoveCompiledFunc) {
  const char* py_src = R"(
def func() -> int:
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
  EXPECT_TRUE(jit_ctx_->removeCompiledFunc(func));
  EXPECT_FALSE(jit_ctx_->removeCompiledFunc(func));
}
class JITFrameTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITFrameTest, MakeFrameReifier) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_TRUE(PyObject_TypeCheck(reifier, (PyTypeObject*)Py_TYPE(reifier)));
  }
}

TEST_F(JITFrameTest, RuntimeFrameStateFromThreadState) {
  PyThreadState* tstate = PyThreadState_Get();
  ASSERT_NE(tstate, nullptr);

  EXPECT_EQ(tstate, PyThreadState_Get());
}

TEST_F(JITFrameTest, CompileAndGetHeader) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_GT(rt->frameSize(), 0);
}

TEST_F(JITFrameTest, ClearExceptCodeOnJitFrame) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
}

TEST_F(JITFrameTest, AfterCompileDidCompile) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  EXPECT_FALSE(jit_ctx_->didCompile(func));

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
}

TEST_F(JITFrameTest, LookupCodeRuntimeBeforeCompile) {
  const char* py_src = R"(
def func():
    return 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  EXPECT_EQ(rt, nullptr);
}

TEST_F(JITFrameTest, FrameSizeAfterCompile) {
  const char* py_src = R"(
def func(a: int, b: int) -> int:
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  EXPECT_GE(compiled->runtime()->frameSize(), 0);
}

TEST_F(JITFrameTest, CompiledCodesNotEmptyAfterCompile) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  const auto before_size = jit_ctx_->compiledCodes().size();

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  const auto& codes_after = jit_ctx_->compiledCodes();
  EXPECT_GT(codes_after.size(), before_size);
}

TEST_F(JITFrameTest, MakeFrameReifierForGenerator) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_TRUE(PyObject_TypeCheck(reifier, (PyTypeObject*)Py_TYPE(reifier)));
  }
}

TEST_F(JITFrameTest, MakeFrameReifierWithArgs) {
  const char* py_src = R"(
def func(a, b, c):
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_NE(reifier, nullptr);
  }
}

TEST_F(JITFrameTest, CompileAndCheckCodeRuntimeFrameState) {
  const char* py_src = R"(
def func(x):
    return x * 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  const auto* fs = rt->frameState();
  ASSERT_NE(fs, nullptr);
  EXPECT_EQ(fs->code(), func->func_code);
  EXPECT_NE(fs->builtins(), nullptr);
  EXPECT_NE(fs->globals(), nullptr);
}

TEST_F(JITFrameTest, CompileAndRunSimpleFunc) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 42);
}

TEST_F(JITFrameTest, CompileAndRunFuncWithArgs) {
  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto a = Ref<>::steal(PyLong_FromLong(10));
  auto b = Ref<>::steal(PyLong_FromLong(20));
  auto args = Ref<>::steal(PyTuple_Pack(2, a.get(), b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 30);
}

TEST_F(JITFrameTest, CompileAndRunGeneratorFunc) {
  const char* py_src = R"(
def gen():
    yield 10
    yield 20
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 10);

  auto second = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(second.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(second), 20);
}

TEST_F(JITFrameTest, MultipleCompiles) {
  const char* py_src = R"(
def func_a():
    return 1

def func_b():
    return 2
)";

  Ref<PyFunctionObject> func_a(compileAndGet(py_src, "func_a"));
  Ref<PyFunctionObject> func_b(compileAndGet(py_src, "func_b"));
  ASSERT_NE(func_a, nullptr);
  ASSERT_NE(func_b, nullptr);

  {
    std::unique_ptr<jit::hir::Preloader> preloader(
        jit::hir::Preloader::makePreloader(
            func_a, jit::makeFrameReifier(func_a->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func_a);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  {
    std::unique_ptr<jit::hir::Preloader> preloader(
        jit::hir::Preloader::makePreloader(
            func_b, jit::makeFrameReifier(func_b->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func_b);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  EXPECT_TRUE(jit_ctx_->didCompile(func_a));
  EXPECT_TRUE(jit_ctx_->didCompile(func_b));
}

class JITPyjitApiTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};


TEST_F(JITPyjitApiTest, CompileFunction) {
  const char* py_src = R"(
def func():
    return 1 + 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileFunctionWithArgs) {
  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileFunctionTwice) {
  const char* py_src = R"(
def func():
    return 99
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result1 = jit::compileFunction(func);
  EXPECT_EQ(result1, jit::Result::OK);

  auto result2 = jit::compileFunction(func);
  EXPECT_EQ(result2, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileGenerator) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, PreloadFuncAndDeps) {
  const char* py_src = R"(
def helper():
    return 10

def func():
    return helper()
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto deps = jit::preloadFuncAndDeps(func, false);
  EXPECT_GE(deps.size(), 1);
}

TEST_F(JITPyjitApiTest, TypeModified) {
  const char* py_src = R"(
class MyClass:
    x = 1
)";

  Ref<> obj(compileAndGet(py_src, "MyClass"));
  ASSERT_NE(obj, nullptr);

  auto type = reinterpret_cast<PyTypeObject*>(obj.get());
  ASSERT_NE(type, nullptr);

  jit::typeModified(type);
}

TEST_F(JITPyjitApiTest, TypeNameModified) {
  const char* py_src = R"(
class MyClass:
    pass
)";

  Ref<> obj(compileAndGet(py_src, "MyClass"));
  ASSERT_NE(obj, nullptr);

  auto type = reinterpret_cast<PyTypeObject*>(obj.get());
  ASSERT_NE(type, nullptr);

  jit::typeNameModified(type);
}

TEST_F(JITPyjitApiTest, FuncModified) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::funcModified(func);
}

TEST_F(JITPyjitApiTest, CompileAndCall) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto call_result =
      Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 42);
}

TEST_F(JITPyjitApiTest, CompileAndCallWithArgs) {
  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);

  auto a = Ref<>::steal(PyLong_FromLong(3));
  auto b = Ref<>::steal(PyLong_FromLong(4));
  auto args = Ref<>::steal(PyTuple_Pack(2, a.get(), b.get()));
  auto call_result =
      Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 7);
}

class JITContextExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITContextExtendedTest, CompiledFuncsAfterCompile) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  const auto& compiled_funcs = jit_ctx_->compiledFuncs();
  EXPECT_FALSE(compiled_funcs.empty());
}

TEST_F(JITContextExtendedTest, LookupCode) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  BorrowedRef<PyCodeObject> code = func->func_code;
  BorrowedRef<PyDictObject> builtins = func->func_builtins;
  BorrowedRef<PyDictObject> globals = func->func_globals;

  auto compiled = jit_ctx_->lookupCode(code, builtins, globals);
  ASSERT_NE(compiled, nullptr);
}

TEST_F(JITContextExtendedTest, CompilePublishesCodeExtraCompiledEntry) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());
  EXPECT_EQ(extra->jit_globals, func->func_globals);
  EXPECT_EQ(extra->jit_builtins, func->func_builtins);
}

TEST_F(JITContextExtendedTest, ForgetCodeClearsCodeExtraCompiledEntry) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());

  jit_ctx_->forgetCode(func);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
}

TEST_F(
    JITContextExtendedTest,
    ClearForMultithreadedCompileTestClearsCodeExtraCompiledEntry) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());

  jit_ctx_->clearForMultithreadedCompileTest();

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
}

TEST_F(
    JITContextExtendedTest,
    ForgetCompiledFunctionClearsCodeExtraCompiledEntry) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());
  ASSERT_NE(func->func_dict, nullptr);

  ASSERT_EQ(
      PyDict_DelItemString(func->func_dict, "__cinderx_compiled_func__"), 0);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
  auto cached = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  EXPECT_EQ(cached, nullptr);
}

TEST_F(JITContextExtendedTest, AddCompileTime) {
  auto before = jit_ctx_->totalCompileTime();
  jit_ctx_->addCompileTime(std::chrono::nanoseconds(1000000));
  auto after = jit_ctx_->totalCompileTime();
  EXPECT_GE(after.count(), before.count());
}

TEST_F(JITContextExtendedTest, IfDeoptStatNotFound) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  ASSERT_NE(rt, nullptr);

  bool called = jit_ctx_->ifDeoptStat(
      rt, 9999, [](const jit::DeoptStat&) { FAIL() << "Should not be called"; });
  EXPECT_FALSE(called);
}

TEST_F(JITContextExtendedTest, ReleaseReferencesAfterAdd) {
  auto obj = Ref<>::steal(PyLong_FromLong(999));
  ASSERT_NE(obj, nullptr);

  jit_ctx_->addReference(obj);
  jit_ctx_->releaseReferences();
}

TEST_F(JITContextExtendedTest, GetAndClearLoadMethodCacheStats) {
  auto stats = jit_ctx_->getAndClearLoadMethodCacheStats();
  EXPECT_TRUE(stats.empty());
}

TEST_F(JITContextExtendedTest, GetAndClearLoadTypeMethodCacheStats) {
  auto stats = jit_ctx_->getAndClearLoadTypeMethodCacheStats();
  EXPECT_TRUE(stats.empty());
}

TEST_F(JITContextExtendedTest, CodeOuterFunctions) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto& outer_funcs = jit_ctx_->codeOuterFunctions();
  EXPECT_GE(outer_funcs.size(), 0u);
}

TEST_F(JITContextExtendedTest, BuiltinsFindByName) {
  const jit::Builtins& builtins = jit_ctx_->builtins();
  auto result = builtins.find("print");
  if (result.has_value()) {
    EXPECT_NE(*result, nullptr);
  }
}

TEST_F(JITContextExtendedTest, BuiltinsFindByMethod) {
  const jit::Builtins& builtins = jit_ctx_->builtins();
  auto meth = builtins.find("len");
  if (meth.has_value() && *meth != nullptr) {
    auto name_result = builtins.find(*meth);
    EXPECT_TRUE(name_result.has_value());
  }
}

TEST_F(JITContextExtendedTest, AllocateAllCaches) {
  auto* la = jit_ctx_->allocateLoadAttrCache();
  auto* lta = jit_ctx_->allocateLoadTypeAttrCache();
  auto* lm = jit_ctx_->allocateLoadMethodCache();
  auto* lma = jit_ctx_->allocateLoadModuleAttrCache();
  auto* lmm = jit_ctx_->allocateLoadModuleMethodCache();
  auto* ltm = jit_ctx_->allocateLoadTypeMethodCache();
  auto* sa = jit_ctx_->allocateStoreAttrCache();

  EXPECT_NE(la, nullptr);
  EXPECT_NE(lta, nullptr);
  EXPECT_NE(lm, nullptr);
  EXPECT_NE(lma, nullptr);
  EXPECT_NE(lmm, nullptr);
  EXPECT_NE(ltm, nullptr);
  EXPECT_NE(sa, nullptr);
}

class JITGenDataFooterTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITGenDataFooterTest, YieldFromValueNotYieldFrom) {
  jit::GenYieldPoint yp(0, jit::kInvalidYieldFromOffset);
  EXPECT_FALSE(yp.isYieldFrom());

  jit::GenDataFooter footer;
  footer.yieldPoint = &yp;

  PyObject* result = jit::yieldFromValue(&footer, &yp);
  EXPECT_EQ(result, nullptr);
}

TEST_F(JITGenDataFooterTest, GenDataFooterDefaults) {
  jit::GenDataFooter footer;
  EXPECT_EQ(footer.linkAddress, 0u);
  EXPECT_EQ(footer.returnAddress, 0u);
  EXPECT_EQ(footer.originalFramePointer, 0u);
  EXPECT_EQ(footer.yieldPoint, nullptr);
  EXPECT_EQ(footer.spillWords, 0u);
  EXPECT_EQ(footer.resumeEntry, nullptr);
  EXPECT_EQ(footer.gen, nullptr);
  EXPECT_EQ(footer.code_rt, nullptr);
}

TEST_F(JITGenDataFooterTest, CompileGeneratorAndCheckRuntime) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  ASSERT_NE(rt, nullptr);
  EXPECT_TRUE(rt->frameState()->isGen());
}

class JITCodeRuntimeExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITCodeRuntimeExtendedTest, AllocateRuntimeFrameState) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  auto* inlined_fs = rt.allocateRuntimeFrameState(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(inlined_fs, nullptr);
  EXPECT_EQ(inlined_fs->code(), func->func_code);
}

TEST_F(JITCodeRuntimeExtendedTest, MultipleDeoptMetadatas) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta1;
  jit::DeoptMetadata meta2;
  std::size_t idx1 = rt.addDeoptMetadata(std::move(meta1));
  std::size_t idx2 = rt.addDeoptMetadata(std::move(meta2));

  EXPECT_EQ(idx1, 0);
  EXPECT_EQ(idx2, 1);
  EXPECT_EQ(rt.deoptMetadatas().size(), 2);
}

TEST_F(JITCodeRuntimeExtendedTest, MultipleGenYieldPoints) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  auto* yp1 = rt.addGenYieldPoint(jit::GenYieldPoint(0, jit::kInvalidYieldFromOffset));
  auto* yp2 = rt.addGenYieldPoint(jit::GenYieldPoint(1, 10));
  ASSERT_NE(yp1, nullptr);
  ASSERT_NE(yp2, nullptr);

  EXPECT_FALSE(yp1->isYieldFrom());
  EXPECT_TRUE(yp2->isYieldFrom());
}

TEST_F(JITCodeRuntimeExtendedTest, RuntimeFrameStateFields) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  const auto* fs = rt.frameState();
  ASSERT_NE(fs, nullptr);
  EXPECT_EQ(fs->code(), func->func_code);
  EXPECT_NE(fs->builtins(), nullptr);
  EXPECT_NE(fs->globals(), nullptr);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionStackSizes) {
  const char* py_src = R"(
def func(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  EXPECT_GE(compiled->stackSize(), 0);
  EXPECT_GE(compiled->spillStackSize(), 0);
  EXPECT_GT(compiled->codeSize(), 0u);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionCompileTime) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  auto compile_time = compiled->compileTime();
  EXPECT_GE(compile_time.count(), 0);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionAddRemoveFunction) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  const char* py_src2 = R"(
def func2():
    return 2
)";

  Ref<PyFunctionObject> func2(compileAndGet(py_src2, "func2"));
  ASSERT_NE(func2, nullptr);

  compiled->addFunction(func2);
  EXPECT_TRUE(compiled->functions().count(func2) > 0);

  compiled->removeFunction(func2);
  EXPECT_TRUE(compiled->functions().count(func2) == 0);
}



class JITCodeRuntimeTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITCodeRuntimeTest, GenYieldPointDeoptIdx) {
  jit::GenYieldPoint yp(42, jit::kInvalidYieldFromOffset);
  EXPECT_EQ(yp.deoptIdx(), 42);
  EXPECT_FALSE(yp.isYieldFrom());
}

TEST_F(JITCodeRuntimeTest, GenYieldPointIsYieldFrom) {
  jit::GenYieldPoint yp(5, 10);
  EXPECT_TRUE(yp.isYieldFrom());
  EXPECT_EQ(yp.yieldFromOffset(), 10);
}

TEST_F(JITCodeRuntimeTest, GenYieldPointSetResumeTarget) {
  jit::GenYieldPoint yp(1, jit::kInvalidYieldFromOffset);
  EXPECT_EQ(yp.resumeTarget(), 0);
  yp.setResumeTarget(0xDEADBEEF);
  EXPECT_EQ(yp.resumeTarget(), 0xDEADBEEF);
}

TEST_F(JITCodeRuntimeTest, RuntimeFrameStateIsGen) {
  const char* py_src = R"(
def normal():
    return 1

def gen():
    yield 1
)";

  Ref<PyFunctionObject> normal_func(compileAndGet(py_src, "normal"));
  Ref<PyFunctionObject> gen_func(compileAndGet(py_src, "gen"));
  ASSERT_NE(normal_func, nullptr);
  ASSERT_NE(gen_func, nullptr);

  {
    jit::CodeRuntime rt{normal_func};
    EXPECT_FALSE(rt.frameState()->isGen());
  }

  {
    jit::CodeRuntime rt{gen_func};
    EXPECT_TRUE(rt.frameState()->isGen());
  }
}


TEST_F(JITCodeRuntimeTest, CodeRuntimeSetFrameSize) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  rt.setFrameSize(128);
  EXPECT_EQ(rt.frameSize(), 128);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeSpillWords) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  EXPECT_EQ(rt.spillWords(), 0);

  rt.setSpillWords(32);
  EXPECT_EQ(rt.spillWords(), 32);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeAddGenYieldPoint) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::GenYieldPoint* yp = rt.addGenYieldPoint(
      jit::GenYieldPoint(0, jit::kInvalidYieldFromOffset));
  ASSERT_NE(yp, nullptr);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeAddDeoptMetadata) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta;
  std::size_t idx = rt.addDeoptMetadata(std::move(meta));
  EXPECT_EQ(idx, 0);

  const jit::DeoptMetadata& stored = rt.getDeoptMetadata(idx);
  EXPECT_EQ(stored.live_values.size(), 0);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeDeoptMetadatas) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta;
  rt.addDeoptMetadata(std::move(meta));

  const auto& metadatas = rt.deoptMetadatas();
  EXPECT_EQ(metadatas.size(), 1);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeDebugInfo) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  jit::DebugInfo* info = rt.debugInfo();
  ASSERT_NE(info, nullptr);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeGetUnitCallStackInvalidIdx) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  auto result = rt.getUnitCallStackFromDeoptIdx(999);
  EXPECT_FALSE(result.has_value());
}


TEST_F(JITCodeRuntimeTest, CodeRuntimeIsCleared) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  EXPECT_FALSE(rt.isCleared());
}

TEST_F(JITCodeRuntimeTest, CompileAndVerifyCodeRuntime) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_GE(rt->frameSize(), 0);
  EXPECT_FALSE(rt->isCleared());

  const auto* frame_state = rt->frameState();
  ASSERT_NE(frame_state, nullptr);
  ASSERT_NE(frame_state->code(), nullptr);
  ASSERT_NE(frame_state->builtins(), nullptr);
  ASSERT_NE(frame_state->globals(), nullptr);
}

class JITGeneratorTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITGeneratorTest, CompileGenerator) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
    yield 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
}

TEST_F(JITGeneratorTest, CompileGeneratorAndCheck) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  EXPECT_FALSE(jit_ctx_->didCompile(func));

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
}

TEST_F(JITGeneratorTest, CompileAndRunGenerator) {
  const char* py_src = R"(
def gen():
    yield 10
    yield 20
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 10);
}

TEST_F(JITGeneratorTest, CompileGeneratorWithArg) {
  const char* py_src = R"(
def gen(n: int):
    for i in range(n):
        yield i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto arg = Ref<>::steal(PyLong_FromLong(3));
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto gen_obj = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  for (int expected = 0; expected < 3; expected++) {
    auto val = Ref<>::steal(PyIter_Next(iter));
    ASSERT_NE(val.get(), nullptr) << "Failed at iteration " << expected;
    EXPECT_EQ(PyLong_AsLong(val), expected);
  }

  auto val = Ref<>::steal(PyIter_Next(iter));
  EXPECT_EQ(val.get(), nullptr);
}



TEST_F(JITGeneratorTest, CompileGeneratorForgetCode) {
  const char* py_src = R"(
def gen():
    yield 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
  EXPECT_TRUE(jit_ctx_->didCompile(func));

  jit_ctx_->forgetCode(func);
  EXPECT_FALSE(jit_ctx_->didCompile(func));
}

TEST_F(JITGeneratorTest, CompileAndIterateGenerator) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
    yield 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  long expected[] = {1, 2, 3};
  for (int i = 0; i < 3; i++) {
    auto val = Ref<>::steal(PyIter_Next(iter));
    ASSERT_NE(val.get(), nullptr) << "Failed at iteration " << i;
    EXPECT_EQ(PyLong_AsLong(val), expected[i]);
  }

  auto val = Ref<>::steal(PyIter_Next(iter));
  EXPECT_EQ(val.get(), nullptr);
}


TEST_F(JITGeneratorTest, CompileCoroutine) {
  const char* py_src = R"(
async def coro():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "coro"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
}

TEST_F(JITGeneratorTest, GeneratorClose) {
  const char* py_src = R"(
def gen():
    try:
        yield 1
        yield 2
    finally:
        pass
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 1);

  auto close_result = PyObject_CallMethod(gen_obj, "close", nullptr);
  EXPECT_NE(close_result, nullptr);
  Py_XDECREF(close_result);
}


TEST_F(JITGeneratorTest, GeneratorRuntimeIsGen) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_TRUE(rt->frameState()->isGen());
}

TEST_F(JITGeneratorTest, NormalFuncRuntimeIsNotGen) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::makePreloader(
          func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_FALSE(rt->frameState()->isGen());
}

namespace {

Ref<> importCinderJitModule() {
  auto mod = Ref<>::steal(PyImport_ImportModule("cinderx.jit"));
  if (mod == nullptr) {
    PyErr_Print();
    throw std::runtime_error("Failed to import cinderx.jit");
  }
  return mod;
}

Ref<> callJitNoArgs(BorrowedRef<> mod, const char* name) {
  auto result = Ref<>::steal(PyObject_CallMethod(mod, name, nullptr));
  if (result == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("cinderx.jit.") + name + " failed");
  }
  return result;
}

Ref<> callJitOneArg(BorrowedRef<> mod, const char* name, PyObject* arg) {
  auto result = Ref<>::steal(PyObject_CallMethod(mod, name, "O", arg));
  if (result == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("cinderx.jit.") + name + " failed");
  }
  return result;
}

Ref<> makeLong(long value) {
  return Ref<>::steal(PyLong_FromLong(value));
}

Ref<> makeList(std::initializer_list<long> values) {
  auto list =
      Ref<>::steal(PyList_New(static_cast<Py_ssize_t>(values.size())));
  if (list == nullptr) {
    return list;
  }
  Py_ssize_t idx = 0;
  for (long value : values) {
    PyObject* item = PyLong_FromLong(value);
    if (item == nullptr) {
      return Ref<>(nullptr);
    }
    PyList_SET_ITEM(list.get(), idx++, item);
  }
  return list;
}

Ref<> call2(PyObject* func, PyObject* arg0, PyObject* arg1) {
  return Ref<>::steal(PyObject_CallFunctionObjArgs(
      func, arg0, arg1, nullptr));
}

std::string pyRepr(BorrowedRef<> obj) {
  auto repr = Ref<>::steal(PyObject_Repr(obj));
  if (repr == nullptr) {
    PyErr_Print();
    throw std::runtime_error("PyObject_Repr failed");
  }
  const char* utf8 = PyUnicode_AsUTF8(repr);
  if (utf8 == nullptr) {
    PyErr_Print();
    throw std::runtime_error("PyUnicode_AsUTF8 failed");
  }
  return utf8;
}

long getLongAttr(BorrowedRef<> obj, const char* name) {
  auto attr = Ref<>::steal(PyObject_GetAttrString(obj, name));
  if (attr == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("missing attribute ") + name);
  }
  return PyLong_AsLong(attr);
}

void expectPyEqual(BorrowedRef<> actual, BorrowedRef<> expected) {
  int equal = PyObject_RichCompareBool(actual, expected, Py_EQ);
  ASSERT_NE(equal, -1) << "comparison failed";
  EXPECT_EQ(equal, 1) << pyRepr(actual) << " != " << pyRepr(expected);
}

void expectPrefixReverseHelperMatchesPython(
    BorrowedRef<> original_func,
    BorrowedRef<> actual,
    BorrowedRef<> expected,
    BorrowedRef<> actual_index,
    BorrowedRef<> expected_index) {
  auto original_result =
      call2(original_func.get(), expected.get(), expected_index.get());
  ASSERT_NE(original_result, nullptr) << "reference Python expression failed";

  ASSERT_EQ(JITRT_ListPrefixReverseAssign(actual.get(), actual_index.get()), 0)
      << "helper failed: " << (PyErr_Occurred() ? "exception set" : "no exception");
  expectPyEqual(actual, original_result);
}

} // namespace

class JITJitRtCoverageTest : public RuntimeTest {};

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperFastPathSemantics) {
  const char* py_src = R"(
def original(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq
)";
  Ref<> original(compileAndGet(py_src, "original"));

  {
    Ref<> index = makeLong(3);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original, makeList({}), makeList({}), index, index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original, makeList({7}), makeList({7}), index, index);
  }
}

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperFallbackSemantics) {
  const char* py_src = R"(
def original(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq

class CountingList(list):
    def __init__(self, value):
        super().__init__(value)
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        return super().__getitem__(key)
    def __setitem__(self, key, value):
        self.set_count += 1
        return super().__setitem__(key, value)

class CustomIndex:
    def __init__(self, value):
        self.value = value
        self.index_count = 0
        self.add_count = 0
    def __index__(self):
        self.index_count += 1
        return self.value
    def __add__(self, other):
        self.add_count += 1
        return self.value + other

class CustomSequence:
    def __init__(self, value):
        self.data = list(value)
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        return self.data[key]
    def __setitem__(self, key, value):
        self.set_count += 1
        self.data[key] = value
    def __eq__(self, other):
        return isinstance(other, CustomSequence) and self.data == other.data
)";
  Ref<> original(compileAndGet(py_src, "original"));
  Ref<> counting_list_type(getGlobal("CountingList"));
  Ref<> custom_index_type(getGlobal("CustomIndex"));
  Ref<> custom_sequence_type(getGlobal("CustomSequence"));

  {
    Ref<> index = makeLong(-1);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2}),
        makeList({0, 1, 2}),
        Ref<>::create(Py_True),
        Ref<>::create(Py_True));
  }
  {
    Ref<> source = makeList({0, 1, 2, 3});
    Ref<> actual = Ref<>::steal(
        PyObject_CallFunctionObjArgs(counting_list_type.get(), source.get(), nullptr));
    Ref<> expected = Ref<>::steal(
        PyObject_CallFunctionObjArgs(counting_list_type.get(), source.get(), nullptr));
    Ref<> index = makeLong(2);
    expectPrefixReverseHelperMatchesPython(original, actual, expected, index, index);
    EXPECT_EQ(getLongAttr(actual, "get_count"), 1);
    EXPECT_EQ(getLongAttr(actual, "set_count"), 1);
  }
  {
    Ref<> actual_index = Ref<>::steal(
        PyObject_CallFunction(custom_index_type.get(), "i", 2));
    Ref<> expected_index = Ref<>::steal(
        PyObject_CallFunction(custom_index_type.get(), "i", 2));
    Ref<> actual = makeList({0, 1, 2, 3});
    Ref<> expected = makeList({0, 1, 2, 3});
    auto original_result =
        call2(original.get(), expected.get(), expected_index.get());
    ASSERT_NE(original_result, nullptr);

    ASSERT_EQ(
        JITRT_ListPrefixReverseAssign(actual.get(), actual_index.get()), 0);
    expectPyEqual(actual, original_result);
    EXPECT_GE(getLongAttr(actual_index, "index_count"), 1);
    EXPECT_EQ(getLongAttr(actual_index, "add_count"), 1);
  }
  {
    Ref<> source = makeList({0, 1, 2, 3});
    Ref<> actual = Ref<>::steal(
        PyObject_CallFunctionObjArgs(custom_sequence_type.get(), source.get(), nullptr));
    Ref<> expected = Ref<>::steal(
        PyObject_CallFunctionObjArgs(custom_sequence_type.get(), source.get(), nullptr));
    Ref<> index = makeLong(2);
    expectPrefixReverseHelperMatchesPython(original, actual, expected, index, index);
    EXPECT_EQ(getLongAttr(actual, "get_count"), 1);
    EXPECT_EQ(getLongAttr(actual, "set_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperExceptionSideEffects) {
  const char* py_src = R"(
class GetError(Exception):
    pass
class AddError(Exception):
    pass
class SetError(Exception):
    pass

class RaisingSeq:
    def __init__(self, phase):
        self.phase = phase
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        if self.phase == "get":
            raise GetError("get")
        return [2, 1, 0]
    def __setitem__(self, key, value):
        self.set_count += 1
        if self.phase == "set":
            raise SetError("set")

class RaisingIndex:
    def __init__(self, phase):
        self.phase = phase
        self.add_count = 0
    def __index__(self):
        return 2
    def __add__(self, other):
        self.add_count += 1
        if self.phase == "add":
            raise AddError("add")
        return 3
)";
  Ref<> raising_seq_type(compileAndGet(py_src, "RaisingSeq"));
  Ref<> raising_index_type(getGlobal("RaisingIndex"));
  Ref<> get_error(getGlobal("GetError"));
  Ref<> add_error(getGlobal("AddError"));
  Ref<> set_error(getGlobal("SetError"));

  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "get"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(get_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 0);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", ""));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", "add"));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(add_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "set"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(set_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 1);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, CompiledListPrefixReverseAssignFastPathSemantics) {
  const char* py_src = R"(
def target(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq
)";
  Ref<PyFunctionObject> target(compileAndGet(py_src, "target"));
  ASSERT_EQ(jit::compileFunction(target), jit::Result::OK);

  {
    Ref<> seq = makeList({0, 1, 2, 3, 4});
    Ref<> index = makeLong(3);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({3, 2, 1, 0, 4}));
  }
  {
    Ref<> seq = makeList({0, 1, 2, 3, 4});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({4, 3, 2, 1, 0}));
  }
  {
    Ref<> seq = makeList({});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({}));
  }
  {
    Ref<> seq = makeList({7});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({7}));
  }
}

TEST_F(JITJitRtCoverageTest, CompiledListPrefixReverseAssignExceptionSideEffects) {
  const char* py_src = R"(
class GetError(Exception):
    pass
class AddError(Exception):
    pass
class SetError(Exception):
    pass

class RaisingSeq:
    def __init__(self, phase):
        self.phase = phase
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        if self.phase == "get":
            raise GetError("get")
        return [2, 1, 0]
    def __setitem__(self, key, value):
        self.set_count += 1
        if self.phase == "set":
            raise SetError("set")

class RaisingIndex:
    def __init__(self, phase):
        self.phase = phase
        self.add_count = 0
    def __index__(self):
        return 2
    def __add__(self, other):
        self.add_count += 1
        if self.phase == "add":
            raise AddError("add")
        return 3

def target(seq, k):
    seq[: k + 1] = seq[k::-1]
)";
  Ref<PyFunctionObject> target(compileAndGet(py_src, "target"));
  Ref<> raising_seq_type(getGlobal("RaisingSeq"));
  Ref<> raising_index_type(getGlobal("RaisingIndex"));
  Ref<> get_error(getGlobal("GetError"));
  Ref<> add_error(getGlobal("AddError"));
  Ref<> set_error(getGlobal("SetError"));
  ASSERT_EQ(jit::compileFunction(target), jit::Result::OK);

  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "get"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(get_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 0);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", ""));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", "add"));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(add_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "set"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(set_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 1);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, CompiledArithmeticUnaryModAndPower) {
  const char* py_src = R"(
def kernel(a, b):
    return (not bool(a), a % b, a ** b, pow(a, b))

def driver():
    return kernel(17, 5)
)";

  Ref<PyFunctionObject> kernel(compileAndGet(py_src, "kernel"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(kernel, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(kernel), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyTuple_Check(result));
  EXPECT_EQ(PyTuple_GET_SIZE(result), 4);
}

TEST_F(JITJitRtCoverageTest, CompiledGlobalNameLoad) {
  const char* py_src = R"(
ANSWER = 321

def get_answer():
    return ANSWER

def run():
    return get_answer()
)";

  Ref<PyFunctionObject> get_answer(compileAndGet(py_src, "get_answer"));
  Ref<PyFunctionObject> run(compileAndGet(py_src, "run"));
  ASSERT_EQ(jit::compileFunction(get_answer), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(run), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 321);
}

TEST_F(JITJitRtCoverageTest, CompiledGeneratorSendAndYieldFrom) {
  const char* py_src = R"(
def inner():
    received = yield 1
    yield received + 10

def outer():
    return (yield from inner())

def drive():
    gen = outer()
    first = next(gen)
    second = gen.send(5)
    return first, second
)";

  Ref<PyFunctionObject> inner(compileAndGet(py_src, "inner"));
  Ref<PyFunctionObject> outer(compileAndGet(py_src, "outer"));
  Ref<PyFunctionObject> drive(compileAndGet(py_src, "drive"));
  ASSERT_EQ(jit::compileFunction(inner), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(outer), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(drive), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(drive, args, nullptr));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyTuple_Check(result));
  EXPECT_EQ(PyTuple_GET_SIZE(result), 2);
  EXPECT_EQ(PyLong_AsLong(PyTuple_GET_ITEM(result.get(), 0)), 1);
  EXPECT_EQ(PyLong_AsLong(PyTuple_GET_ITEM(result.get(), 1)), 15);
}

TEST_F(JITJitRtCoverageTest, CompiledVectorcallEntry) {
  const char* py_src = R"(
def callee(a, b, c):
    return a + b + c

def caller():
    return callee(1, 2, 3)
)";

  Ref<PyFunctionObject> callee(compileAndGet(py_src, "callee"));
  Ref<PyFunctionObject> caller(compileAndGet(py_src, "caller"));
  ASSERT_EQ(jit::compileFunction(callee), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(caller), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(caller, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITJitRtCoverageTest, CompiledContainersComparisonsAndBitOps) {
  const char* py_src = R"(
def mix(a, b):
    bits = ((a & b) | (a ^ b)) << 1
    values = [a, b, bits, a + b]
    values[1] += 3
    data = {"a": values[0], "b": values[1], "bits": bits}
    part = values[1:4]
    if data["a"] < data["b"] and bits != 0:
        return part[0] + part[1] + part[2] + data.get("bits", 0)
    return -1

def driver():
    return mix(3, 6)
)";

  Ref<PyFunctionObject> mix(compileAndGet(py_src, "mix"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(mix, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(mix), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

TEST_F(JITJitRtCoverageTest, CompiledAttributesMethodsAndLoops) {
  const char* py_src = R"(
class Box:
    def __init__(self, value):
        self.value = value

    def add(self, other):
        self.value += other
        return self.value

def work(limit):
    box = Box(1)
    total = 0
    i = 0
    while i < limit:
        total += box.add(i)
        i += 1
    return total + box.value

def driver():
    return work(8)
)";

  Ref<PyFunctionObject> work(compileAndGet(py_src, "work"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(work, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(work), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

TEST_F(JITJitRtCoverageTest, CompiledBroadOpcodeShapes) {
  const char* py_src = R"(
class Base:
    def value(self):
        return 3

class Child(Base):
    def value(self):
        return super().value() + 4

def unicode_ops(text, n):
    return (text + "!" * n)[1:4] + str(n)

def dict_set_ops(a, b):
    d = {"a": a, **{"b": b}}
    e = {"c": a + b}
    d.update(e)
    s = {a, b}
    s.update({a + b})
    return d["a"] + d["b"] + d["c"] + len(s)

def match_ops(obj):
    match obj:
        case {"kind": "pair", "left": left, "right": right}:
            return left + right
        case [first, second, *rest]:
            return first + second + len(rest)
        case _:
            return 0

def unpack_ops(seq):
    first, *middle, last = seq
    return first + last + len(middle)

def format_ops(a, b):
    return f"{a}:{b!r}:{a + b}"

def driver():
    child = Child()
    return (
        len(unicode_ops("abcdef", 3))
        + dict_set_ops(2, 5)
        + match_ops({"kind": "pair", "left": 4, "right": 6})
        + match_ops([1, 2, 3, 4])
        + unpack_ops((1, 2, 3, 4))
        + len(format_ops(3, 8))
        + child.value()
    )
)";

  const char* names[] = {
      "unicode_ops",
      "dict_set_ops",
      "match_ops",
      "unpack_ops",
      "format_ops",
      "driver",
  };
  for (const char* name : names) {
    Ref<PyFunctionObject> func(compileAndGet(py_src, name));
    ASSERT_NE(func, nullptr) << name;
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK) << name;
  }
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

class JITPyjitApiCoverageTest : public RuntimeTest {};

TEST_F(JITPyjitApiCoverageTest, EnableDisableAndQueryState) {
  auto mod = importCinderJitModule();
  callJitNoArgs(mod, "enable");
  auto enabled = callJitNoArgs(mod, "is_enabled");
  EXPECT_TRUE(PyObject_IsTrue(enabled));

  callJitNoArgs(mod, "disable");
  auto disabled = callJitNoArgs(mod, "is_enabled");
  EXPECT_FALSE(PyObject_IsTrue(disabled));

  callJitNoArgs(mod, "enable");
}

TEST_F(JITPyjitApiCoverageTest, CompileLazyForceAndUncompile) {
  const char* py_src = R"(
def sample(x):
    return x + 7
)";

  Ref<PyFunctionObject> sample(compileAndGet(py_src, "sample"));
  auto mod = importCinderJitModule();

  callJitOneArg(mod, "lazy_compile", sample);
  auto not_compiled = callJitOneArg(mod, "is_jit_compiled", sample);
  EXPECT_FALSE(PyObject_IsTrue(not_compiled));

  auto arg = Ref<>::steal(PyLong_FromLong(10));
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto call_result = Ref<>::steal(PyObject_Call(sample, args, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 17);

  callJitOneArg(mod, "force_compile", sample);
  auto compiled = callJitOneArg(mod, "is_jit_compiled", sample);
  EXPECT_TRUE(PyObject_IsTrue(compiled));

  callJitOneArg(mod, "force_uncompile", sample);
}

TEST_F(JITPyjitApiCoverageTest, JitListDisassembleAndSuppress) {
  runStockCode(R"(
import cinderx.jit as jit
import os
import tempfile

def listed_fn(x):
    return x * 3

jit.enable()
jit.append_jit_list("jittestmodule:listed_fn")
jit.get_jit_list()

with tempfile.NamedTemporaryFile("w", delete=False) as f:
    f.write("jittestmodule:listed_fn\n")
    path = f.name
try:
    jit.read_jit_list(path)
finally:
    os.unlink(path)

jit.force_compile(listed_fn)
jit.disassemble(listed_fn)

@jit.jit_suppress
def suppressed(y):
    return y + 1

jit.jit_unsuppress(suppressed)
jit.force_uncompile(listed_fn)
)");
}

TEST_F(JITPyjitApiCoverageTest, AutoCompileAfterNCallsAndStats) {
  runStockCode(R"(
import cinderx.jit as jit

def counted(x):
    return x + 2

jit.enable()
jit.auto()
jit.compile_after_n_calls(1)
assert counted(4) == 6
calls = jit.count_interpreted_calls(counted)
assert calls >= 0
funcs = jit.get_compiled_functions()
assert isinstance(funcs, list)
jit.clear_runtime_stats()
stats = jit.get_and_clear_runtime_stats()
assert stats is not None
)");
}

class JITContextP0CoverageTest : public RuntimeTest {};

TEST_F(JITContextP0CoverageTest, GlobalContextStrBuildClassAndDeoptedFuncs) {
  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  BorrowedRef<> build_class = ctx->strBuildClass();
  ASSERT_NE(build_class, nullptr);
  EXPECT_TRUE(PyUnicode_Check(build_class));

  const auto& deopted = ctx->deoptedFuncs();
  EXPECT_GE(deopted.size(), 0u);
}

TEST_F(JITContextP0CoverageTest, GlobalContextForgetCodeAndReferences) {
  const char* py_src = R"(
def forget_me():
    return 88
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "forget_me"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(ctx->didCompile(func));

  auto holder = Ref<>::steal(PyLong_FromLong(42));
  ctx->addReference(holder);
  ctx->releaseReferences();

  ctx->forgetCode(func);
  EXPECT_FALSE(ctx->didCompile(func));
}

class JITStaticJitRtCoverageTest : public RuntimeTest {
 public:
  JITStaticJitRtCoverageTest()
      : RuntimeTest(static_cast<Flags>(kJit | kStaticCompiler)) {}
};

TEST_F(JITStaticJitRtCoverageTest, StaticPrimitiveBoxUnboxModPow) {
  const char* py_src = R"(
from __static__ import int32, int64, uint32, uint64

def work() -> int64:
    a: int32 = int32(17)
    b: int32 = int32(5)
    m: int32 = a % b
    u: uint32 = uint32(9) % uint32(4)
    p: int64 = int64(m * m)
    q: uint64 = uint64(u * u)
    return p + int64(q)

def run() -> int64:
    return work()
)";

  Ref<PyFunctionObject> work(compileStaticAndGet(py_src, "work"));
  Ref<PyFunctionObject> run(compileStaticAndGet(py_src, "run"));
  ASSERT_NE(work, nullptr);
  ASSERT_NE(run, nullptr);
  ASSERT_EQ(jit::compileFunction(work), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(run), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_NE(PyLong_AsLong(result), 0);
}

TEST_F(JITStaticJitRtCoverageTest, StaticArrayFieldsAndInvoke) {
  const char* py_src = R"(
from __static__ import Array, box, clen, int64

class Accum:
    value: int64

    def __init__(self, value: int64):
        self.value = value

    def add(self, other: int64) -> int64:
        self.value = self.value + other
        return self.value

def array_total(size: int) -> int:
    arr: Array[int64] = Array[int64](size)
    i: int64 = 0
    while i < clen(arr):
        arr[i] = i + int64(1)
        i += 1
    arr[int64(0)] = int64(7)
    total: int64 = 0
    for value in arr:
        total += value
    total += arr[int64(0)]
    total += arr[1]
    return box(total)

def invoke_total() -> int:
    acc = Accum(int64(5))
    first: int64 = acc.add(int64(3))
    second: int64 = acc.add(int64(4))
    return box(first + second + acc.value)

def run() -> int:
    return array_total(5) + invoke_total()
)";

  const char* names[] = {"array_total", "invoke_total", "run"};
  for (const char* name : names) {
    Ref<PyFunctionObject> func(compileStaticAndGet(py_src, name));
    ASSERT_NE(func, nullptr) << name;
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK) << name;
  }

  Ref<PyFunctionObject> run(compileStaticAndGet(py_src, "run"));
  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}
