// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/inliner.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
// NOLINTNEXTLINE(facebook-hte-BadInclude-regex)
#include <regex>
#include <sstream>

#ifdef BUCK_BUILD
#include "tools/cxx/Resources.h"
#endif

using namespace jit;
using namespace jit::lir;

namespace jit::codegen {

class BackendTest : public RuntimeTest {
 public:
  // compile a function without generating prologue and epilogue.
  // the function is self-contained.
  // this function is used to test LIR, rewrite passes, register allocation,
  // and machine code generation.
  void* SimpleCompile(
      Function* lir_func,
      int arg_buffer_size = 0,
      size_t* code_size = nullptr) {
    Environ environ;
    InitEnviron(environ);
    PostGenerationRewrite post_gen(lir_func, &environ);
    post_gen.run();

    LinearScanAllocator lsalloc(lir_func);
    lsalloc.run();

    environ.shadow_frames_and_spill_size = lsalloc.getFrameSize();
    environ.changed_regs = lsalloc.getChangedRegs();

    PostRegAllocRewrite post_rewrite(lir_func, &environ);
    post_rewrite.run();

    asmjit::CodeHolder code;
    ICodeAllocator* code_allocator =
        cinderx::getModuleState()->code_allocator.get();
    code.init(code_allocator->asmJitEnvironment());

    arch::Builder as(&code);

    environ.as = &as;

#if defined(CINDER_X86_64)
    as.push(asmjit::x86::rbp);
    as.mov(asmjit::x86::rbp, asmjit::x86::rsp);
#elif defined(CINDER_AARCH64)
    as.stp(arch::fp, arch::lr, asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
    as.mov(arch::fp, asmjit::a64::sp);
#else
    CINDER_UNSUPPORTED
#endif

    auto saved_regs = environ.changed_regs & CALLEE_SAVE_REGS;

#if defined(CINDER_X86_64)
    int saved_regs_size = saved_regs.count() * 8;
#elif defined(CINDER_AARCH64)
    int saved_regs_size = saved_regs.count() * 16;
#else
    CINDER_UNSUPPORTED
    int saved_regs_size = saved_regs.count() * 8;
#endif

    // Allocate stack space for the function's stack.
    // Allocate 8 bytes for the function's stack.
    // If the stack size is not a multiple of 16, add 8 bytes to the stack size.
    // This is to ensure that the stack is aligned to 16 bytes.

    int allocate_stack = std::max(environ.shadow_frames_and_spill_size, 8);
    if ((allocate_stack + saved_regs_size + arg_buffer_size) % 16 != 0) {
      allocate_stack += 8;
    }

#if defined(CINDER_X86_64)
    // Allocate stack space and save the size of the function's stack.
    as.sub(asmjit::x86::rsp, allocate_stack);

    // Push used callee-saved registers.
    std::vector<int> pushed_regs;
    pushed_regs.reserve(saved_regs.count());
    while (!saved_regs.Empty()) {
      as.push(asmjit::x86::gpq(saved_regs.GetFirst().loc));
      pushed_regs.push_back(saved_regs.GetFirst().loc);
      saved_regs.RemoveFirst();
    }

    if (arg_buffer_size > 0) {
      as.sub(asmjit::x86::rsp, arg_buffer_size);
    }

    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    gen.env_ = std::move(environ);
    gen.lir_func_.reset(lir_func);
    gen.generateAssemblyBody(code);

    if (arg_buffer_size > 0) {
      as.add(asmjit::x86::rsp, arg_buffer_size);
    }

    for (auto riter = pushed_regs.rbegin(); riter != pushed_regs.rend();
         ++riter) {
      as.pop(asmjit::x86::gpq(*riter));
    }

    as.leave();
    as.ret();
#elif defined(CINDER_AARCH64)
    // Allocate stack space and save the size of the function's stack.
    JIT_CHECK(allocate_stack % kStackAlign == 0, "unaligned");
    as.sub(asmjit::a64::sp, asmjit::a64::sp, allocate_stack);

    // Push used callee-saved registers, handling GP and FP separately.
    auto gp_regs = saved_regs & ALL_GP_REGISTERS;
    auto vecd_regs = saved_regs & ALL_VECD_REGISTERS;

    std::vector<int> pushed_gp_regs;
    pushed_gp_regs.reserve(gp_regs.count());
    while (!gp_regs.Empty()) {
      as.str(
          asmjit::a64::x(gp_regs.GetFirst().loc),
          asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
      pushed_gp_regs.push_back(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
    }

    std::vector<int> pushed_vecd_regs;
    pushed_vecd_regs.reserve(vecd_regs.count());
    while (!vecd_regs.Empty()) {
      as.str(
          asmjit::a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
          asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
      pushed_vecd_regs.push_back(vecd_regs.GetFirst().loc);
      vecd_regs.RemoveFirst();
    }

    if (arg_buffer_size > 0) {
      JIT_CHECK(arg_buffer_size % kStackAlign == 0, "unaligned");
      as.sub(asmjit::a64::sp, asmjit::a64::sp, arg_buffer_size);
    }

    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    gen.env_ = std::move(environ);
    gen.lir_func_.reset(lir_func);
    gen.generateAssemblyBody(code);

    if (arg_buffer_size > 0) {
      as.add(asmjit::a64::sp, asmjit::a64::sp, arg_buffer_size);
    }

    for (auto riter = pushed_vecd_regs.rbegin();
         riter != pushed_vecd_regs.rend();
         ++riter) {
      as.ldr(
          asmjit::a64::d(*riter - VECD_REG_BASE),
          asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    }

    for (auto riter = pushed_gp_regs.rbegin(); riter != pushed_gp_regs.rend();
         ++riter) {
      as.ldr(
          asmjit::a64::x(*riter), asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    }

    as.mov(asmjit::a64::sp, arch::fp);
    as.ldp(arch::fp, arch::lr, asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    as.ret(arch::lr);
#else
    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    CINDER_UNSUPPORTED
#endif

    as.finalize();

    if (code_size != nullptr) {
      *code_size = code.codeSize();
    }
    AllocateResult result = code_allocator->addCode(&code);
    EXPECT_EQ(result.error, asmjit::kErrorOk);
    EXPECT_TRUE(code_allocator->contains(result.addr))
        << "Compiled function should exist within the CodeAllocator";
    Function* caller_owned_lir_func = gen.lir_func_.release();
    EXPECT_EQ(caller_owned_lir_func, lir_func);
    return result.addr;
  }

  void InitEnviron(Environ& environ) {
    for (const auto& loc : ARGUMENT_REGS) {
      environ.arg_locations.push_back(loc);
    }
  }

#if defined(CINDER_AARCH64)
  std::string DisassembleLIRFunction(Function* lir_func) {
    Environ environ;
    InitEnviron(environ);

    PostGenerationRewrite post_gen(lir_func, &environ);
    post_gen.run();

    LinearScanAllocator lsalloc(lir_func);
    lsalloc.run();

    environ.shadow_frames_and_spill_size = lsalloc.getFrameSize();
    environ.changed_regs = lsalloc.getChangedRegs();

    PostRegAllocRewrite post_rewrite(lir_func, &environ);
    post_rewrite.run();

    auto code_allocator =
        std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
    asmjit::CodeHolder code;
    code.init(code_allocator->asmJitEnvironment());

    asmjit::a64::Builder as(&code);
    environ.as = &as;

    as.stp(arch::fp, arch::lr, asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
    as.mov(arch::fp, asmjit::a64::sp);

    NativeGeneratorFactory factory;
    auto gen = factory(nullptr);
    gen->env_ = std::move(environ);
    gen->lir_func_.reset(lir_func);
    gen->generateAssemblyBody(code);
    Function* caller_owned_lir_func = gen->lir_func_.release();
    EXPECT_EQ(caller_owned_lir_func, lir_func);

    as.mov(asmjit::a64::sp, arch::fp);
    as.ldp(arch::fp, arch::lr, asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    as.ret(arch::lr);
    as.finalize();

    JIT_CHECK(code.flatten() == asmjit::kErrorOk, "failed to flatten code");
    JIT_CHECK(
        code.resolveUnresolvedLinks() == asmjit::kErrorOk,
        "failed to resolve code links");

    std::ostringstream out;
    auto section = code.sectionById(0);
    Disassembler dis{
        reinterpret_cast<const char*>(section->data()), section->bufferSize()};
    dis.setPrintAddr(false);
    dis.setPrintInstBytes(false);
    dis.disassembleAll(out);
    return out.str();
  }

  // Exercise the complete pipeline used by the new pre-RA AArch64 rules.
  // SimpleCompile intentionally starts after target selection because most
  // backend tests construct already-selected LIR.
  void* CompileAfterTargetSelect(
      Function* lir_func,
      size_t* code_size = nullptr) {
    selectTargetOpcodes(lir_func);
    return SimpleCompile(lir_func, 0, code_size);
  }

  // Compile pre-allocated LIR (physical registers + stack slots) directly to
  // machine code, bypassing register allocation.  Used for tests that need
  // precise control over which registers and stack slots are used.
  void* CompilePreAllocated(
      Function* lir_func,
      int spill_size,
      size_t* code_size = nullptr) {
    Environ environ;
    InitEnviron(environ);

    // Skip PostGenerationRewrite and LinearScanAllocator — the instructions
    // are already in post-alloc form with physical registers and stack slots.
    environ.shadow_frames_and_spill_size = spill_size;
    environ.changed_regs = {};

    PostRegAllocRewrite post_rewrite(lir_func, &environ);
    post_rewrite.run();

    asmjit::CodeHolder code;
    ICodeAllocator* code_allocator =
        cinderx::getModuleState()->code_allocator.get();
    code.init(code_allocator->asmJitEnvironment());

    arch::Builder as(&code);
    environ.as = &as;

    // Prologue: save frame pointer and link register, set up frame.
    as.stp(arch::fp, arch::lr, asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
    as.mov(arch::fp, asmjit::a64::sp);

    // Allocate stack space for spill slots.
    int allocate_stack = spill_size;
    if (allocate_stack % kStackAlign != 0) {
      allocate_stack += kStackAlign - (allocate_stack % kStackAlign);
    }
    as.sub(asmjit::a64::sp, asmjit::a64::sp, allocate_stack);

    NativeGeneratorFactory factory;
    auto gen = factory(nullptr);
    gen->env_ = std::move(environ);
    gen->lir_func_.reset(lir_func);
    gen->generateAssemblyBody(code);

    // Epilogue: restore stack and frame pointer, return.
    as.mov(asmjit::a64::sp, arch::fp);
    as.ldp(arch::fp, arch::lr, asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    as.ret(arch::lr);

    as.finalize();

    if (code_size != nullptr) {
      *code_size = code.codeSize();
    }
    AllocateResult result = code_allocator->addCode(&code);
    EXPECT_EQ(result.error, asmjit::kErrorOk);
    Function* caller_owned_lir_func = gen->lir_func_.release();
    EXPECT_EQ(caller_owned_lir_func, lir_func);
    return result.addr;
  }
#endif

  void CheckCast(Function* lir_func) {
    auto func =
        (PyObject * (*)(PyObject*, PyTypeObject*)) SimpleCompile(lir_func);

    auto test_noerror = [&](PyObject* a_in, PyTypeObject* b_in) -> void {
      auto ret_test = func(a_in, b_in);
      ASSERT_TRUE(PyErr_Occurred() == nullptr);
      auto ret_jitrt = JITRT_Cast(a_in, b_in);
      ASSERT_TRUE(PyErr_Occurred() == nullptr);
      ASSERT_EQ(ret_test, ret_jitrt);
    };

    auto test_error = [&](PyObject* a_in, PyTypeObject* b_in) -> void {
      auto ret_test = func(a_in, b_in);
      ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
      PyErr_Clear();

      auto ret_jitrt = JITRT_Cast(a_in, b_in);
      ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
      PyErr_Clear();

      ASSERT_EQ(ret_test, ret_jitrt);
    };

    test_noerror(Py_False, &PyBool_Type);
    test_noerror(Py_False, &PyLong_Type);
    test_error(Py_False, &PyUnicode_Type);
  }
};

// This is a test harness for experimenting with backends
TEST_F(BackendTest, SimpleLoadAttr) {
  const char* src = R"(
class User:
  def __init__(self, user_id):
    self._user_id = user_id

def get_user_id(user):
    return user._user_id
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  // Borrowed from locals
  PyObject* get_user_id = PyDict_GetItemString(locals, "get_user_id");
  ASSERT_NE(get_user_id, nullptr) << "Couldn't get get_user_id function";

  // Borrowed from get_user_id
  // code holds the code object for the function
  // code->co_consts holds the constants referenced by LoadConst
  // code->co_names holds the names referenced by LoadAttr
  PyObject* code = PyFunction_GetCode(get_user_id);
  ASSERT_NE(code, nullptr) << "Couldn't get code for user_id";

  // At this point you could patch user_id->vectorcall with a pointer to
  // your generated code for get_user_id.
  //
  // The HIR should be:
  //
  // fun get_user_id {
  //   bb 0 {
  //     CheckVar a0
  //     t0 = LoadAttr a0 0
  //     CheckExc t0
  //     Incref t0
  //     Return t0
  //   }
  // }

  // Create a user object we can use to call our function
  PyObject* user_klass = PyDict_GetItemString(locals, "User");
  ASSERT_NE(user_klass, nullptr) << "Couldn't get class User";

  auto user_id = Ref<>::steal(PyLong_FromLong(12345));
  ASSERT_NE(user_id.get(), nullptr) << "Couldn't create user id";

  auto user = Ref<>::steal(
      PyObject_CallFunctionObjArgs(user_klass, user_id.get(), nullptr));
  ASSERT_NE(user.get(), nullptr) << "Couldn't create user";

  // Finally, call get_user_id
  auto result = Ref<>::steal(
      PyObject_CallFunctionObjArgs(get_user_id, user.get(), nullptr));
  ASSERT_NE(result.get(), nullptr) << "Failed getting user id";
  ASSERT_TRUE(PyLong_CheckExact(result)) << "Incorrect type returned";
  ASSERT_EQ(PyLong_AsLong(result), PyLong_AsLong(user_id))
      << "Incorrect user id returned";
}

TEST_F(BackendTest, CallCountTest) {
  const char* src = R"(
def foo(x: int) -> int:
  return x + 1

for i in range(30):
  foo(i)
)";

  Ref<> foo = compileAndGet(src, "foo");
  ASSERT_TRUE(PyFunction_Check(foo));

  BorrowedRef<PyCodeObject> code =
      reinterpret_cast<PyFunctionObject*>(foo.get())->func_code;

  auto extra = codeExtra(code);
  ASSERT_NE(extra, nullptr) << "Failed to load code object extra data";
  uint64_t ncalls = Ci_code_extra_get_calls(extra);

  // TASK(T190615535): This is waiting on the 3.12 custom interpreter loop.
  // Once we have that in place, we can start incrementing call counts in 3.12.
  ASSERT_EQ(ncalls, 30);
}

TEST_F(BackendTest, ExplicitLIRSubKeepsRhsRegisterLiveAcrossOutputDefine) {
#if !defined(CINDER_X86_64)
  GTEST_SKIP() << "x86_64-specific allocator/codegen repro";
#else
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  constexpr PhyLocation kLhsReg = R10;
  constexpr std::array<PhyLocation, 13> kPressureRegs = {
      RCX, RDX, RBX, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15};

  std::vector<Instruction*> pressure;
  pressure.reserve(kPressureRegs.size());
  // Bind vregs to almost every GP register without emitting code so the
  // allocator has to make a real choice at the Sub instruction.
  for (PhyLocation reg : kPressureRegs) {
    pressure.push_back(bb->allocateInstr(
        Instruction::kBind,
        nullptr,
        OutVReg{OperandBase::k64bit},
        PhyReg{reg, OperandBase::k64bit}));
  }

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kLhsReg, OperandBase::k64bit},
      Imm{7});
  auto rhs = bb->allocateInstr(
      Instruction::kMove, nullptr, OutVReg{OperandBase::k64bit}, Imm{3});
  auto sub = bb->allocateInstr(
      Instruction::kSub,
      nullptr,
      OutVReg{OperandBase::k64bit},
      PhyReg{kLhsReg, OperandBase::k64bit},
      VReg{rhs});
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, OperandBase::k64bit},
      VReg{sub});
  bb->allocateInstr(Instruction::kReturn, nullptr);
  bb->addSuccessor(epilogue);
  // Keep the bound registers live across the Sub by threading them into the
  // successor block. Without this, the pressure would end before the bug site.
  for (Instruction* live_out : pressure) {
    epilogue->allocateInstr(
        Instruction::kPhi,
        nullptr,
        OutVReg{OperandBase::k64bit},
        Lbl{bb},
        VReg{live_out});
  }

  auto func = reinterpret_cast<uint64_t (*)()>(SimpleCompile(lirfunc.get()));

  ASSERT_TRUE(sub->output()->isReg());
  ASSERT_TRUE(sub->getInput(1)->isReg());
  // Before the fix, both operands could land in RAX here, producing
  // `mov rax, r10; sub rax, rax` and returning 0 instead of 4.
  EXPECT_NE(
      sub->output()->getPhyRegister(), sub->getInput(1)->getPhyRegister());
  EXPECT_EQ(func(), 4);
#endif
}

// floating-point arithmetic test
TEST_F(BackendTest, FPArithmetic) {
  double a = 3.12;
  double b = 1.1616;

  auto test = [&](Instruction::Opcode opcode) -> double {
    auto lirfunc = std::make_unique<Function>();
    auto bb = lirfunc->allocateBasicBlock();

    auto pa = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&a)));
    auto fa = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pa));

    auto pb = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&b)));
    auto fb = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pb));

    auto sum = bb->allocateInstr(
        opcode, nullptr, OutVReg(OperandBase::kDouble), VReg(fa), VReg(fb));
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_double_return_loc, OperandBase::kDouble},
        VReg(sum));
    bb->allocateInstr(Instruction::kReturn, nullptr);

    // need this because the register allocator assumes the basic blocks
    // end with Return should have one and only one successor.
    auto epilogue = lirfunc->allocateBasicBlock();
    bb->addSuccessor(epilogue);

    auto func = (double (*)())SimpleCompile(lirfunc.get());

    return func();
  };

  ASSERT_DOUBLE_EQ(test(Instruction::kFadd), a + b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFsub), a - b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFmul), a * b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFdiv), a / b);
}

TEST_F(BackendTest, FPCompare) {
  double a = 3.12;
  double b = 1.1616;

  auto test = [&](Instruction::Opcode opcode) -> double {
    auto lirfunc = std::make_unique<Function>();
    auto bb = lirfunc->allocateBasicBlock();

    auto pa = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&a)));
    auto fa = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pa));

    auto pb = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&b)));
    auto fb = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pb));

    auto compare =
        bb->allocateInstr(opcode, nullptr, OutVReg(), VReg(fa), VReg(fb));
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc},
        VReg(compare));
    bb->allocateInstr(Instruction::kReturn, nullptr);

    // need this because the register allocator assumes the basic blocks
    // end with Return should have one and only one successor.
    auto epilogue = lirfunc->allocateBasicBlock();
    bb->addSuccessor(epilogue);

    auto func = (bool (*)())SimpleCompile(lirfunc.get());

    return func();
  };

  ASSERT_DOUBLE_EQ(test(Instruction::kEqual), a == b);
  ASSERT_DOUBLE_EQ(test(Instruction::kNotEqual), a != b);
  ASSERT_DOUBLE_EQ(test(Instruction::kGreaterThanUnsigned), a > b);
  ASSERT_DOUBLE_EQ(test(Instruction::kLessThanUnsigned), a < b);
  ASSERT_DOUBLE_EQ(test(Instruction::kGreaterThanEqualUnsigned), a >= b);
  ASSERT_DOUBLE_EQ(test(Instruction::kLessThanEqualUnsigned), a <= b);
}

#if defined(CINDER_AARCH64)

namespace {

std::string disassembleAArch64Snippet(
    const std::function<void(asmjit::a64::Builder*)>& emit) {
  auto code_allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  asmjit::CodeHolder code;
  code.init(code_allocator->asmJitEnvironment());

  asmjit::a64::Builder as(&code);
  emit(&as);
  as.ret(arch::lr);
  as.finalize();

  JIT_CHECK(code.flatten() == asmjit::kErrorOk, "failed to flatten code");
  JIT_CHECK(
      code.resolveUnresolvedLinks() == asmjit::kErrorOk,
      "failed to resolve code links");

  std::ostringstream out;
  auto section = code.sectionById(0);
  Disassembler dis{
      reinterpret_cast<const char*>(section->data()), section->bufferSize()};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  return out.str();
}

uint64_t runBranchBitAndReadNzcv(
    Instruction::Opcode opcode,
    uint64_t value,
    uint64_t nzcv) {
  JIT_CHECK(
      opcode == Instruction::kBranchBitSet ||
          opcode == Instruction::kBranchBitNotSet,
      "expected a BranchBit opcode");

  auto code_allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  asmjit::CodeHolder code;
  code.init(code_allocator->asmJitEnvironment());
  arch::Builder as(&code);

  Environ environ;
  environ.as = &as;

  Function function;
  BasicBlock source(&function);
  BasicBlock target(&function);
  auto target_label = as.newLabel();
  environ.block_label_map.emplace(&target, target_label);

  auto* branch = source.allocateInstr(
      opcode,
      nullptr,
      PhyReg{arch::reg_general_return_loc, DataType::k64bit},
      Imm{31});
  branch->addOperands(Lbl{&target});

  auto done = as.newLabel();
  as.msr(asmjit::a64::Predicate::SysReg::kNZCV, asmjit::a64::x1);
  autogen::AutoTranslator::getInstance().translateInstr(&environ, branch);
  as.mov(asmjit::a64::x2, 0);
  as.b(done);
  as.bind(target_label);
  as.mov(asmjit::a64::x2, 1);
  as.bind(done);
  as.mrs(asmjit::a64::x0, asmjit::a64::Predicate::SysReg::kNZCV);
  as.orr(asmjit::a64::x0, asmjit::a64::x0, asmjit::a64::x2);
  as.ret(arch::lr);

  JIT_CHECK(as.finalize() == asmjit::kErrorOk, "failed to finalize code");
  AllocateResult result = code_allocator->addCode(&code);
  JIT_CHECK(result.error == asmjit::kErrorOk, "failed to allocate code");

  auto func = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(result.addr);
  return func(value, nzcv);
}

} // namespace

TEST_F(BackendTest, BranchBitHasNoAArch64FlagEffects) {
  EXPECT_EQ(
      InstrProperty::getProperties(Instruction::kBranchBitSet).flag_effects,
      FlagEffects::kNone);
  EXPECT_EQ(
      InstrProperty::getProperties(Instruction::kBranchBitNotSet).flag_effects,
      FlagEffects::kNone);
}

TEST_F(BackendTest, BranchBitPreservesNzcv) {
  constexpr uint64_t kNzcv = 0xa0000000;
  constexpr uint64_t kBit31 = uint64_t{1} << 31;
  constexpr uint64_t kTaken = 1;

  EXPECT_EQ(
      runBranchBitAndReadNzcv(Instruction::kBranchBitSet, kBit31, kNzcv),
      kNzcv | kTaken);
  EXPECT_EQ(
      runBranchBitAndReadNzcv(Instruction::kBranchBitSet, 0, kNzcv), kNzcv);
  EXPECT_EQ(
      runBranchBitAndReadNzcv(Instruction::kBranchBitNotSet, 0, kNzcv),
      kNzcv | kTaken);
  EXPECT_EQ(
      runBranchBitAndReadNzcv(Instruction::kBranchBitNotSet, kBit31, kNzcv),
      kNzcv);
}

TEST_F(BackendTest, SplitAddSubImmediate) {
  constexpr uint64_t kSplitImm = 8193;

  auto test = [&](Instruction::Opcode opcode, uint64_t arg) -> uint64_t {
    auto lirfunc = std::make_unique<Function>();
    auto bb = lirfunc->allocateBasicBlock();

    auto input =
        bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
    auto result = bb->allocateInstr(
        opcode, nullptr, OutVReg(), VReg(input), Imm(kSplitImm));
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc},
        VReg(result));
    bb->allocateInstr(Instruction::kReturn, nullptr);

    auto epilogue = lirfunc->allocateBasicBlock();
    bb->addSuccessor(epilogue);

    auto func = (uint64_t (*)(uint64_t))SimpleCompile(lirfunc.get());
    return func(arg);
  };

  ASSERT_EQ(test(Instruction::kAdd, 7), 8200);
  ASSERT_EQ(test(Instruction::kSub, 8200), 7);
}

TEST_F(BackendTest, SplitEqualImmediate) {
  constexpr uint64_t kSplitImm = 8193;

  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  auto input =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto result = bb->allocateInstr(
      Instruction::kEqual, nullptr, OutVReg(), VReg(input), Imm(kSplitImm));
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(result));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  auto func = (bool (*)(uint64_t))SimpleCompile(lirfunc.get());
  ASSERT_TRUE(func(kSplitImm));
  ASSERT_FALSE(func(kSplitImm - 1));
}

TEST_F(BackendTest, SplitImmediateMachineCodeShape) {
  auto add_disasm = disassembleAArch64Snippet([](asmjit::a64::Builder* as) {
    arch::add_immediate(as, asmjit::a64::x0, asmjit::a64::x0, 8193);
  });
  EXPECT_TRUE(
      std::regex_search(add_disasm, std::regex{"add\\s+x0, x0, #2, lsl #12"}))
      << add_disasm;
  EXPECT_TRUE(std::regex_search(add_disasm, std::regex{"add\\s+x0, x0, #1"}))
      << add_disasm;
  EXPECT_EQ(add_disasm.find("x13"), std::string::npos) << add_disasm;

  auto sub_disasm = disassembleAArch64Snippet([](asmjit::a64::Builder* as) {
    arch::sub_immediate(as, asmjit::a64::x0, asmjit::a64::x0, 8193);
  });
  EXPECT_TRUE(
      std::regex_search(sub_disasm, std::regex{"sub\\s+x0, x0, #2, lsl #12"}))
      << sub_disasm;
  EXPECT_TRUE(std::regex_search(sub_disasm, std::regex{"sub\\s+x0, x0, #1"}))
      << sub_disasm;
  EXPECT_EQ(sub_disasm.find("x13"), std::string::npos) << sub_disasm;
}

TEST_F(BackendTest, SmallAndUnsplitImmediateMachineCodeShape) {
  auto small_disasm = disassembleAArch64Snippet([](asmjit::a64::Builder* as) {
    arch::add_immediate(as, asmjit::a64::x0, asmjit::a64::x0, 4095);
  });
  EXPECT_TRUE(std::regex_search(
      small_disasm, std::regex{"add\\s+x0, x0, #(4095|0xfff)"}))
      << small_disasm;
  EXPECT_EQ(small_disasm.find("x13"), std::string::npos) << small_disasm;

  auto unsplit_disasm = disassembleAArch64Snippet([](asmjit::a64::Builder* as) {
    arch::add_immediate(as, asmjit::a64::x0, asmjit::a64::x0, 1 << 24);
  });
  EXPECT_NE(unsplit_disasm.find("x13"), std::string::npos) << unsplit_disasm;
  EXPECT_TRUE(
      std::regex_search(unsplit_disasm, std::regex{"add\\s+x0, x0, x13"}))
      << unsplit_disasm;
}

TEST_F(BackendTest, CmpImmediateAvoidsClobberingScratchInput) {
  auto disasm = disassembleAArch64Snippet([](asmjit::a64::Builder* as) {
    arch::cmp_immediate(as, arch::reg_scratch_0, 1 << 24);
  });

  EXPECT_NE(disasm.find("x14"), std::string::npos) << disasm;
  EXPECT_EQ(disasm.find("mov x13"), std::string::npos) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"cmp\\s+x13, x14"}))
      << disasm;
}

TEST_F(BackendTest, TreeIterTwoInputImmediateLoadsArgDirectly) {
  Parser parser;
  auto lirfunc = parser.parse(R"(Function:
BB %0
       %1:Object = Move 4096(0x1000):Object
       %2:64bit = StateStackPush %1:Object, 1(0x1):32bit
       %3:64bit = Move 0(0x0):64bit
                   Return %3:64bit
)");
  auto epilogue = lirfunc->allocateBasicBlock();
  lirfunc->basicblocks()[0]->addSuccessor(epilogue);

  auto disasm = DisassembleLIRFunction(lirfunc.get());
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"mov\\s+x2, #1"})) << disasm;
  EXPECT_EQ(disasm.find("mov x14, #1"), std::string::npos) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"mov\\s+x0, x29"}))
      << disasm;
}

#endif // CINDER_AARCH64

namespace {
double rt_func(
    int a,
    int b,
    int c,
    int d,
    int e,
    double fa,
    double fb,
    double fc,
    double fd,
    double fe,
    double ff,
    double fg,
    double fh,
    double fi,
    int f,
    int g,
    int h,
    double fj) {
  return fj + a + b + c + d + e + fa * fb * fc * fd * fe * ff * fg * fh * fi +
      f + g + h;
}

template <typename... Arg>
struct AllocateOperand;

template <typename Arg, typename... Args>
struct AllocateOperand<Arg, Args...> {
  Instruction* instr;
  explicit AllocateOperand(Instruction* i) : instr(i) {}

  void operator()(Arg arg, Args... args) {
    if constexpr (std::is_same_v<int, Arg>) {
      instr->allocateImmediateInput(arg);
    } else {
      instr->allocateFPImmediateInput(arg);
    }

    (AllocateOperand<Args...>(instr))(args...);
  }
};

template <>
struct AllocateOperand<> {
  Instruction* instr;
  explicit AllocateOperand(Instruction* i) : instr(i) {}

  void operator()() {}
};

template <typename... Ts>
auto getAllocateOperand(Instruction* instr, std::tuple<Ts...>) {
  return AllocateOperand<Ts...>(instr);
}
} // namespace

TEST_F(BackendTest, ManyArguments) {
  auto args = std::make_tuple(
      1,
      2,
      3,
      4,
      5,
      1.1,
      2.2,
      3.3,
      4.4,
      5.5,
      6.6,
      7.7,
      8.8,
      9.9,
      6,
      7,
      8,
      10.1);

  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  Instruction* call = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(rt_func)));

  std::apply(getAllocateOperand(call, args), args);

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  // need this because the register allocator assumes the basic blocks
  // end with Return should have one and only one successor.
  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  constexpr int kArgBufferSize = 32; // 4 arguments need to pass by stack
  auto func = (double (*)())SimpleCompile(lirfunc.get(), kArgBufferSize);

  double expected = std::apply(rt_func, args);
  double result = func();

  ASSERT_DOUBLE_EQ(result, expected);
}

#if defined(CINDER_AARCH64)
namespace {
uint64_t tenIntegerArguments(
    uint64_t a0,
    uint64_t a1,
    uint64_t a2,
    uint64_t a3,
    uint64_t a4,
    uint64_t a5,
    uint64_t a6,
    uint64_t a7,
    uint64_t a8,
    uint64_t a9) {
  return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7 +
      9 * a8 + 10 * a9;
}
} // namespace

TEST_F(BackendTest, AArch64StackArgumentImmediateStoresFallBackEndToEnd) {
  constexpr uint64_t kArgs[] = {1, 2, 3, 4, 5, 6, 7, 8, 42, 42};

  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* call = block->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg{DataType::k64bit},
      Imm{reinterpret_cast<uint64_t>(tenIntegerArguments)});
  for (uint64_t arg : kArgs) {
    call->allocateImmediateInput(arg, DataType::k64bit);
  }
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      VReg{call});
  block->allocateInstr(Instruction::kReturn, nullptr);
  auto* epilogue = lirfunc->allocateBasicBlock();
  block->addSuccessor(epilogue);

  size_t code_size = 0;
  auto* raw_func = SimpleCompile(lirfunc.get(), 16, &code_size);
  auto func = reinterpret_cast<uint64_t (*)()>(raw_func);
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);
  EXPECT_EQ(
      func(),
      tenIntegerArguments(
          kArgs[0],
          kArgs[1],
          kArgs[2],
          kArgs[3],
          kArgs[4],
          kArgs[5],
          kArgs[6],
          kArgs[7],
          kArgs[8],
          kArgs[9]));

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(raw_func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  // rewriteRegularFunction keeps small immediate stack arguments as
  // immediate-to-memory Moves.  They therefore do not satisfy the
  // register-source StorePair guard.  Exercise that real ABI path and require
  // the conservative scalar fallback; the direct SP-base StorePair test below
  // separately verifies the eligible register-source lowering.
  EXPECT_FALSE(std::regex_search(
      disasm, std::regex{"stp\\s+(x[0-9]+),\\s*\\1,\\s*\\[sp(?:,|\\])"}))
      << disasm;
  EXPECT_TRUE(
      std::regex_search(disasm, std::regex{"str\\s+x[0-9]+,\\s*\\[sp\\]"}))
      << disasm;
  EXPECT_TRUE(std::regex_search(
      disasm, std::regex{"str\\s+x[0-9]+,\\s*\\[sp,\\s*#8\\]"}))
      << disasm;
}

TEST_F(BackendTest, AArch64StackStorePairExecutesWithDistinctSources) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* lower_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{SP, 0, DataType::kObject},
      PhyReg{ARGUMENT_REGS[0], DataType::k64bit});
  lower_store->output()->setDataType(DataType::kObject);
  auto* upper_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{SP, 8, DataType::kObject},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit});
  upper_store->output()->setDataType(DataType::kObject);

  size_t code_size = 0;
  auto* raw_func = SimpleCompile(lirfunc.get(), 16, &code_size);
  auto func = reinterpret_cast<void (*)(uint64_t, uint64_t)>(raw_func);
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  // Returning after the local stack write validates that the pair did not
  // corrupt the frame record or stack restoration.
  func(0x1122334455667788, 0x99aabbccddeeff00);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(raw_func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  const std::regex pair{"stp\\s+x0,\\s*x1,\\s*\\[sp(?:,|\\])"};
  EXPECT_EQ(
      std::distance(
          std::sregex_iterator(disasm.begin(), disasm.end(), pair),
          std::sregex_iterator()),
      1)
      << disasm;
}
#endif

namespace {
static double add(double a, double b) {
  return a + b;
}
} // namespace

TEST_F(BackendTest, FPMultipleCalls) {
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  double a = 1.1;
  double b = 2.2;
  double c = 3.3;
  double d = 4.4;

  auto loadFP = [&](double* n) {
    auto m1 = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(n)));
    auto m2 = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(m1));
    return m2;
  };

  auto la = loadFP(&a);
  auto lb = loadFP(&b);
  auto sum1 = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(la),
      VReg(lb));

  auto lc = loadFP(&c);
  auto ld = loadFP(&d);
  auto sum2 = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(lc),
      VReg(ld));

  auto sum = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(sum1),
      VReg(sum2));

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_double_return_loc, OperandBase::kDouble},
      VReg(sum));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  auto func = (double (*)())SimpleCompile(lirfunc.get());
  double result = func();

  ASSERT_DOUBLE_EQ(result, a + b + c + d);
}

TEST_F(BackendTest, MoveSequenceOptTest) {
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk(-16),
      PhyReg(arch::reg_scratch_0_loc));
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutStk(-24), PhyReg(ARGUMENT_REGS[1].loc));
  bb->allocateInstr(
      lir::Instruction::kMove,
      nullptr,
      OutStk(-32),
      PhyReg(ARGUMENT_REGS[3].loc));

  auto call = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm(0),
      lir::Stk(-16),
      lir::Stk(-24),
      lir::Stk(-32));
  call->getInput(3)->setLastUse();

  Environ env;
  PostRegAllocRewrite post_rewrite(lirfunc.get(), &env);
  post_rewrite.run();

  /*
  BB %0
  [RBP - 16]:Object = Move RAX:Object
  [RBP - 24]:Object = Move RSI:Object
        RDI:Object = Move RAX:Object
        RSI:Object = Move [RBP - 24]:Object
        RDX:Object = Move RCX:Object
                     Call Object

  [RBP - 32] is deleted: lastUse with no later stack reads.
  RSI = Move [RBP - 24] is a self-reload (RSI spilled and loaded back to RSI).
  It is left intact because reg == out_reg skips the rewrite.
  */
  ASSERT_EQ(bb->getNumInstrs(), 6);
  auto& instrs = bb->instructions();

  auto iter = instrs.begin();

  auto* spill0 = (*(iter++)).get();
  auto* spill1 = (*(iter++)).get();
  auto* arg0 = (*(iter++)).get();
  auto* arg1 = (*(iter++)).get();
  auto* arg2 = (*(iter++)).get();
  auto* call_instr = (*(iter++)).get();

  ASSERT_EQ(spill0->opcode(), Instruction::kMove);
  ASSERT_EQ(spill1->opcode(), Instruction::kMove);
  ASSERT_EQ(arg0->opcode(), Instruction::kMove);
  ASSERT_EQ(arg0->getInput(0)->type(), OperandBase::kReg);
  ASSERT_EQ(arg1->opcode(), Instruction::kMove);
  ASSERT_EQ(arg1->getInput(0)->type(), OperandBase::kStack);
  ASSERT_EQ(arg2->opcode(), Instruction::kMove);
  ASSERT_EQ(call_instr->opcode(), Instruction::kCall);
}

TEST_F(BackendTest, MoveSequenceOpt2Test) {
  // OptimizeMoveSequence should not set reg operands that are also output
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk(-16),
      PhyReg(arch::reg_general_return_loc));

  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg(arch::reg_general_return_loc),
      PhyReg(ARGUMENT_REGS[1].loc),
      lir::Stk(-16));

  Environ env;
  PostRegAllocRewrite post_rewrite(lirfunc.get(), &env);
  post_rewrite.run();

  /*
  BB %0
  [RBP - 16]:Object = Move RAX:Object
        RAX:Object = Add RSI:Object, [RBP - 16]:Object
  */
#if defined(CINDER_AARCH64)
  ASSERT_EQ(bb->getNumInstrs(), 3);
  auto& instrs = bb->instructions();

  auto iter = instrs.begin();

  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  auto stack_load = (*(iter++)).get();
  ASSERT_EQ(stack_load->opcode(), Instruction::kMove);
  ASSERT_TRUE(stack_load->output()->isReg());
  ASSERT_NE(
      stack_load->output()->getPhyRegister(), arch::reg_general_return_loc);

  auto add = (*iter).get();
  ASSERT_EQ(add->opcode(), Instruction::kAdd);
  ASSERT_TRUE(add->getInput(1)->isReg());
  ASSERT_EQ(
      add->getInput(1)->getPhyRegister(),
      stack_load->output()->getPhyRegister());
#else
  ASSERT_EQ(bb->getNumInstrs(), 2);
  auto& instrs = bb->instructions();

  auto iter = instrs.begin();

  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*iter)->opcode(), Instruction::kAdd);
  ASSERT_EQ((*iter)->getInput(1)->type(), OperandBase::kStack);
#endif
}

TEST_F(BackendTest, MoveSequenceOptLeavesSelfReloadsIntact) {
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  const PhyLocation kSharedSlot{-16, 64};
  const PhyLocation kReloadReg = ARGUMENT_REGS[0];
  constexpr uint64_t kExpected = 4;

  // Set up the previously failing case:
  //
  //   [RBP - 16] = Move RSI
  //          RSI = Move [RBP - 16]   ; writes RSI, does not consume cached RSI
  //          RAX = Move [RBP - 16]   ; later stack read still needs the spill
  //
  // A bad rewrite would turn the middle instruction into `RSI = Move RSI` and
  // then conclude the spill is dead. This test checks that we keep both the
  // spill store and the explicit self-reload in the block.
  // Make a deleted spill observable instead of reading arbitrary stack data.
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk{kSharedSlot, OperandBase::k64bit},
      Imm{kExpected - kExpected, OperandBase::k64bit});
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReloadReg, OperandBase::k64bit},
      Imm{kExpected, OperandBase::k64bit});
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk{kSharedSlot, OperandBase::k64bit},
      PhyReg{kReloadReg, OperandBase::k64bit});

  auto self_reload = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReloadReg, OperandBase::k64bit},
      Stk{kSharedSlot, OperandBase::k64bit});
  self_reload->getInput(0)->setLastUse();
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, OperandBase::k64bit},
      Stk{kSharedSlot, OperandBase::k64bit});
  bb->allocateInstr(Instruction::kReturn, nullptr);
  bb->addSuccessor(epilogue);

  auto func = reinterpret_cast<uint64_t (*)()>(SimpleCompile(lirfunc.get()));

  bool saw_spill = false;
  bool saw_self_reload = false;
  for (auto& instr : bb->instructions()) {
    if (!instr->isMove()) {
      continue;
    }
    auto* out = instr->output();
    auto* in = instr->getInput(0);
    if (out->isStack() && out->getStackSlot().loc == kSharedSlot.loc &&
        in->isReg() && in->getPhyRegister() == kReloadReg) {
      saw_spill = true;
    }
    if (out->isReg() && out->getPhyRegister() == kReloadReg && in->isStack() &&
        in->getStackSlot().loc == kSharedSlot.loc) {
      saw_self_reload = true;
    }
  }

  EXPECT_TRUE(saw_spill);
  EXPECT_TRUE(saw_self_reload);
  EXPECT_EQ(func(), kExpected);
}

TEST_F(BackendTest, CastTest) {
  // constants used to print out error
  static const char* errmsg = "expected '%s', got '%s'";

  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();
  auto bb3 = lirfunc->allocateBasicBlock();
  auto bb4 = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  // BB 1 : Py_TYPE(ob) == (tp)
  auto a =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto b =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));

  auto a_tp = bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(a, offsetof(PyObject, ob_type)));
  auto eq1 = bb1->allocateInstr(
      Instruction::kEqual, nullptr, OutVReg(), VReg(a_tp), VReg(b));
  bb1->allocateInstr(Instruction::kCondBranch, nullptr, VReg(eq1));
  bb1->addSuccessor(bb3); // true
  bb1->addSuccessor(bb2); // false

  // BB2 : PyType_IsSubtype(Py_TYPE(ob), (tp))
  auto subtype = bb2->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(PyType_IsSubtype)),
      VReg(a_tp),
      VReg(b));
  bb2->allocateInstr(Instruction::kCondBranch, nullptr, VReg(subtype));
  bb2->addSuccessor(bb3); // true
  bb2->addSuccessor(bb4); // false

  // BB3 : return object
  bb3->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(a));
  bb3->allocateInstr(Instruction::kReturn, nullptr);
  bb3->addSuccessor(epilogue);

  // BB4 : return null
  auto a_name = bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(a_tp, offsetof(PyTypeObject, tp_name)));
  auto b_name = bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(b, offsetof(PyTypeObject, tp_name)));
  bb4->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm(reinterpret_cast<uint64_t>(PyErr_Format)),
      Imm(reinterpret_cast<uint64_t>(PyExc_TypeError)),
      Imm(reinterpret_cast<uint64_t>(errmsg)),
      VReg(b_name),
      VReg(a_name));
  auto nll = bb4->allocateInstr(Instruction::kMove, nullptr, OutVReg(), Imm(0));
  bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(nll));
  bb4->allocateInstr(Instruction::kReturn, nullptr);
  bb4->addSuccessor(epilogue);

  CheckCast(lirfunc.get());
}

TEST_F(BackendTest, ParserStringInputTest) {
  auto lir_str = fmt::format(R"(Function:
BB %0 - succs: %4
        %1:Object = Move "hello"
        Return %1:Object

BB %4 - preds: %0

)");
  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  auto func = (char* (*)())SimpleCompile(parsed_func.get());
  std::string ret = func();
  ASSERT_EQ(ret, "hello");
}

TEST_F(BackendTest, ParserMultipleStringInputTest) {
  auto lir_str = fmt::format(R"(Function:
BB %0 - succs: %8
        %1:Object = Move "hello1"
        %2:Object = Move "hello2"
        %3:Object = Move "hello3"
        %4:Object = Move "hello4"
        %5:Object = Move "hello5"
        %6:Object = Move "hello6"
                    Return %1:Object

BB %8 - preds: %0

)");
  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  auto func = (char* (*)())SimpleCompile(parsed_func.get());
  std::string ret = func();
  ASSERT_EQ(ret, "hello1");
}

TEST_F(BackendTest, SplitBasicBlockTest) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();
  auto bb3 = lirfunc->allocateBasicBlock();
  auto bb4 = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  auto r1 =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  bb1->allocateInstr(Instruction::kCondBranch, nullptr, VReg(r1));
  bb1->addSuccessor(bb2);
  bb1->addSuccessor(bb3);

  auto r2 = bb2->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r1), Imm(8));
  bb2->addSuccessor(bb4);

  auto r3 = bb3->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r1), Imm(8));
  auto r4 = bb3->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r3), Imm(8));
  bb3->addSuccessor(bb4);

  auto r5 = bb4->allocateInstr(
      Instruction::kPhi,
      nullptr,
      OutVReg(),
      Lbl(bb2),
      VReg(r2),
      Lbl(bb3),
      VReg(r4));
  bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(r5));
  bb4->allocateInstr(Instruction::kReturn, nullptr);
  bb4->addSuccessor(epilogue);

  // split blocks and then test that function output is still correct
  auto bb_new = bb1->splitBefore(r1);
  bb_new->splitBefore(r1); // test that bb_new is valid
  bb2->splitBefore(r2); // test fixupPhis
  auto bb_nullptr = bb2->splitBefore(r3); // test instruction not in block
  ASSERT_EQ(bb_nullptr, nullptr);
  bb3->splitBefore(r4); // test split in middle of block

  auto func = (uint64_t (*)(int64_t))SimpleCompile(lirfunc.get());

  ASSERT_EQ(func(0), 16);
  ASSERT_EQ(func(1), 9);
}

TEST_F(BackendTest, InlineJITRTCastTest) {
  Function caller;
  auto bb = caller.allocateBasicBlock();
  auto r1 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto r2 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));
  auto call_instr = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(JITRT_Cast)),
      VReg(r1),
      VReg(r2));
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call_instr));
  bb->allocateInstr(Instruction::kReturn, nullptr);
  auto epilogue = caller.allocateBasicBlock();
  bb->addSuccessor(epilogue);
  LIRInliner inliner{&caller, call_instr};
  inliner.inlineCall();

  // Check that caller LIR is as expected.
  auto expected_caller = fmt::format(
      R"(Function:
BB %0 - succs: %8
       %1:Object = LoadArg 0(0x0):64bit
       %2:Object = LoadArg 1(0x1):64bit

BB %8 - preds: %0 - succs: %10 %9
      %15:Object = Move [%1:Object + 0x8]:Object
      %16:Object = Equal %15:Object, %2:Object
                   CondBranch %16:Object

BB %9 - preds: %8 - succs: %10 %11
      %18:Object = Call {0}({0:#x}):Object, %15:Object, %2:Object
                   CondBranch %18:Object

BB %11 - preds: %9 - succs: %12
      %22:Object = Move [%15:Object + 0x18]:Object
      %23:Object = Move [%2:Object + 0x18]:Object
                   Call {1}({1:#x}):Object, {2}({2:#x}):Object, string_literal, %23:Object, %22:Object
      %25:Object = Move 0(0x0):Object

BB %10 - preds: %8 %9 - succs: %12

BB %12 - preds: %10 %11 - succs: %7
      %28:Object = Phi (BB%10, %1:Object), (BB%11, %25:Object)

BB %7 - preds: %12 - succs: %6
       %3:Object = Move %28:Object
{3:>16} = Move %3:Object
                   Return

BB %6 - preds: %7

)",
      reinterpret_cast<uint64_t>(PyType_IsSubtype),
      reinterpret_cast<uint64_t>(PyErr_Format),
      reinterpret_cast<uint64_t>(PyExc_TypeError),
      fmt::format("{}:Object", arch::reg_general_return_loc.toString()));
  std::stringstream ss;
  caller.sortBasicBlocks();
  ss << caller;
  // Replace the string literal address
  std::regex reg(R"(\d+\(0x[0-9a-fA-F]+\):Object, %23:Object, %22:Object)");
  std::string caller_str =
      regex_replace(ss.str(), reg, "string_literal, %23:Object, %22:Object");
  ASSERT_EQ(expected_caller, caller_str);

  // Test execution of caller
  CheckCast(&caller);
}

TEST_F(BackendTest, PostgenJITRTCastTest) {
  auto caller = std::make_unique<Function>();
  auto bb = caller->allocateBasicBlock();
  auto r1 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto r2 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));
  auto call_instr = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(JITRT_Cast)),
      VReg(r1),
      VReg(r2));
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call_instr));
  bb->allocateInstr(Instruction::kReturn, nullptr);
  auto epilogue = caller->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  Environ environ;
  InitEnviron(environ);
  PostGenerationRewrite post_gen(caller.get(), &environ);
  post_gen.run();

  // Check that caller LIR is as expected.
  auto expected_caller = fmt::format(
      R"(Function:
BB %0 - succs: %8
       %1:Object = Bind {0}:Object
       %2:Object = Bind {1}:Object

BB %8 - preds: %0 - succs: %10 %9
      %15:Object = Move [%1:Object + 0x8]:Object
      %16:Object = Equal %15:Object, %2:Object
                   CondBranch %16:Object

BB %9 - preds: %8 - succs: %10 %11
)"
      R"(      %18:Object = Call {2}({2:#x}):Object, %15:Object, %2:Object
)"
      R"(                   CondBranch %18:Object

BB %11 - preds: %9 - succs: %12
      %22:Object = Move [%15:Object + 0x18]:Object
      %23:Object = Move [%2:Object + 0x18]:Object
)"
      R"(                   Call {3}({3:#x}):Object, {4}({4:#x}):Object, string_literal, %23:Object, %22:Object
)"
      R"(      %25:Object = Move 0(0x0):Object

BB %10 - preds: %8 %9 - succs: %12

BB %12 - preds: %10 %11 - succs: %7
      %28:Object = Phi (BB%10, %1:Object), (BB%11, %25:Object)

BB %7 - preds: %12 - succs: %6
       %3:Object = Move %28:Object
{5:>16} = Move %3:Object
                   Return

BB %6 - preds: %7

)",
      ARGUMENT_REGS[0],
      ARGUMENT_REGS[1],
      reinterpret_cast<uint64_t>(PyType_IsSubtype),
      reinterpret_cast<uint64_t>(PyErr_Format),
      reinterpret_cast<uint64_t>(PyExc_TypeError),
      fmt::format("{}:Object", arch::reg_general_return_loc.toString()));
  std::stringstream ss;
  caller->sortBasicBlocks();
  ss << *caller;
  // Replace the string literal address
  std::regex reg(R"(\d+\(0x[0-9a-fA-F]+\):Object, %23:Object, %22:Object)");
  std::string caller_str =
      regex_replace(ss.str(), reg, "string_literal, %23:Object, %22:Object");
  ASSERT_EQ(expected_caller, caller_str);
}

TEST_F(BackendTest, ParserErrorFromExpectTest) {
  // Test throw from expect
  Parser parser;
  parser.parse(R"(Function:
BB %0
)");
  try {
    // Bad basic block header
    parser.parse(R"(Function:
BB %0 %3
)");
    FAIL();
  } catch (ParserException&) {
  }

  try {
    // Dupicate ID
    parser.parse(R"(Function:
BB %0
%1:Object = Bind RDI:Object
%1:Object
)");
    FAIL();
  } catch (ParserException&) {
  }
}

TEST_F(BackendTest, ParserErrorFromMapGetTest) {
  // Test throw from map_get_throw
  Parser parser;
  try {
    // Invalid opcode
    parser.parse(R"(Function:
BB %0
%1:Object = InvalidInstruction
)");
    FAIL();
  } catch (ParserException&) {
  }
  try {
    // Missing basic block
    parser.parse(R"(Function:
BB %0 - succs: %2
Return 0(0x0):Object
BB %1
)");
    FAIL();
  } catch (ParserException&) {
  }
}

#if defined(CINDER_AARCH64)
// This test uses CompilePreAllocated to construct the exact instruction
// sequence the buggy register allocator would emit:
//   1. Store a 64-bit pointer in X19 and a 32-bit flag in X21
//   2. Swap X19↔X21 using X13 as temp with kObject-width moves
//   3. Return X21 (which should hold the original 64-bit pointer)
//
// With the fix (k64bit moves): the pointer is preserved in full.
// Without the fix (k32bit moves): upper 32 bits are zeroed.
TEST_F(BackendTest, RegSwapPreserves64BitPointers) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();

  // Use caller-saved registers that don't clash with argument registers
  // or the scratch register (X13). X9 and X10 are available.
  constexpr auto kReg_A = X9;
  constexpr auto kReg_B = X10;

  // BB1: Move arg0 (64-bit pointer) to X19, arg1 (32-bit flag) to X21.
  // Then perform a 3-register swap X19↔X21 using X13 (scratch) as temp,
  // emitting with kObject width — this is what the FIXED regalloc emits.
  // (The buggy version would use k32bit for the second edge, truncating.)
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{ARGUMENT_REGS[0], DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k32bit});

  // Swap X19↔X21 via X13, using k64bit (the fix) — preserves all 64 bits.
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_scratch_0_loc, DataType::kObject},
      PhyReg{kReg_A, DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::kObject},
      PhyReg{arch::reg_scratch_0_loc, DataType::kObject});

  bb1->allocateInstr(Instruction::kBranch, nullptr, Lbl{bb2});
  bb1->addSuccessor(bb2);

  // BB2: Return X21 (should hold the original 64-bit pointer from X19).
  bb2->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});

  auto func = (uint64_t(*)(uint64_t, uint64_t))CompilePreAllocated(
      lirfunc.release(), 16);
  ASSERT_NE(func, nullptr);

  // The pointer has non-zero upper 32 bits.
  // If the swap truncated it, upper bits would be zero.
  constexpr uint64_t kPtr = 0xDEADBEEFCAFEBABEULL;
  constexpr uint64_t kFlag = 42;
  uint64_t result = func(kPtr, kFlag);
  EXPECT_EQ(result, kPtr) << "Register swap truncated 64-bit pointer: got 0x"
                          << std::hex << result << ", expected 0x" << kPtr;

  // Also verify the upper 32 bits survived.
  EXPECT_EQ(result & 0xFFFFFFFF00000000ULL, 0xDEADBEEF00000000ULL)
      << "Upper 32 bits of pointer destroyed during swap: got 0x" << std::hex
      << result;
}

// Negative test: verify that k32bit swap moves DO truncate 64-bit values.
// This confirms the bug pattern — if this test ever passes, the k32bit
// codegen changed and the fix in rewriteLIREmitCopies may need revisiting.
TEST_F(BackendTest, RegSwapK32bitTruncates64BitValues) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();

  constexpr auto kReg_A = X9;
  constexpr auto kReg_B = X10;

  // Same setup as RegSwapPreserves64BitPointers, but swap uses k32bit
  // (the buggy data type that the unfixed regalloc would emit).
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{ARGUMENT_REGS[0], DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k32bit});

  // Swap using k32bit — this SHOULD truncate the 64-bit pointer.
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_scratch_0_loc, DataType::k32bit},
      PhyReg{kReg_A, DataType::k32bit});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::k32bit},
      PhyReg{kReg_B, DataType::k32bit});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{arch::reg_scratch_0_loc, DataType::k32bit});

  bb1->allocateInstr(Instruction::kBranch, nullptr, Lbl{bb2});
  bb1->addSuccessor(bb2);

  bb2->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});

  auto func = (uint64_t(*)(uint64_t, uint64_t))CompilePreAllocated(
      lirfunc.release(), 16);
  ASSERT_NE(func, nullptr);

  constexpr uint64_t kPtr = 0xDEADBEEFCAFEBABEULL;
  uint64_t result = func(kPtr, 42);

  // The k32bit swap SHOULD truncate — upper 32 bits should be zero.
  // This confirms the bug pattern exists in the codegen layer.
  EXPECT_NE(result, kPtr)
      << "k32bit swap should NOT preserve full 64-bit value";
  EXPECT_EQ(result & 0xFFFFFFFF00000000ULL, 0ULL)
      << "k32bit swap should zero upper 32 bits, got 0x" << std::hex << result;
}

TEST_F(BackendTest, AArch64ZeroRegisterStoresPreserveCanaries) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  struct StoreCase {
    int32_t offset;
    DataType type;
    size_t width;
  };
  constexpr StoreCase kStores[] = {
      {1, DataType::k8bit, 1},
      {4, DataType::k16bit, 2},
      {8, DataType::k32bit, 4},
      {16, DataType::k64bit, 8},
      {32, DataType::kObject, 8},
  };

  for (const auto& store_case : kStores) {
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], store_case.offset, store_case.type},
        Imm{0, store_case.type});
    store->output()->setDataType(store_case.type);
  }

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 48> bytes;
  bytes.fill(0xA5);
  func(bytes.data());

  std::array<bool, 48> should_be_zero{};
  for (const auto& store_case : kStores) {
    for (size_t i = 0; i < store_case.width; i++) {
      should_be_zero[store_case.offset + i] = true;
    }
  }
  for (size_t i = 0; i < bytes.size(); i++) {
    EXPECT_EQ(bytes[i], should_be_zero[i] ? 0 : 0xA5)
        << "unexpected byte at offset " << i;
  }

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();

  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rb\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rh\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?r\\s+wzr"})) << disasm;
  const std::regex xzr_store{"stu?r\\s+xzr"};
  const auto xzr_store_count = std::distance(
      std::sregex_iterator(disasm.begin(), disasm.end(), xzr_store),
      std::sregex_iterator());
  EXPECT_GE(xzr_store_count, 2) << disasm;
  EXPECT_FALSE(std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64ZeroRegisterStoresFoldMaterializationsEndToEnd) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  struct StoreCase {
    PhyLocation temporary;
    int32_t offset;
    DataType type;
    size_t width;
  };
  constexpr StoreCase kStores[] = {
      {X8, 1, DataType::k8bit, 1},
      {X9, 3, DataType::k16bit, 2},
      {X10, 7, DataType::k32bit, 4},
      {X11, 15, DataType::k64bit, 8},
      {X12, 31, DataType::kObject, 8},
  };

  for (const auto& store_case : kStores) {
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{store_case.temporary, store_case.type},
        Imm{0, store_case.type});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], store_case.offset, store_case.type},
        PhyReg{store_case.temporary, store_case.type});
    store->output()->setDataType(store_case.type);
    store->getInput(0)->setLastUse();
  }

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 48> bytes;
  bytes.fill(0xA5);
  func(bytes.data());

  std::array<bool, 48> should_be_zero{};
  for (const auto& store_case : kStores) {
    for (size_t i = 0; i < store_case.width; i++) {
      should_be_zero[store_case.offset + i] = true;
    }
  }
  for (size_t i = 0; i < bytes.size(); i++) {
    EXPECT_EQ(bytes[i], should_be_zero[i] ? 0 : 0xA5)
        << "unexpected byte at offset " << i;
  }

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();

  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rb\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rh\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?r\\s+wzr"})) << disasm;
  const std::regex xzr_store{"stu?r\\s+xzr"};
  const auto xzr_store_count = std::distance(
      std::sregex_iterator(disasm.begin(), disasm.end(), xzr_store),
      std::sregex_iterator());
  EXPECT_GE(xzr_store_count, 2) << disasm;
  EXPECT_FALSE(std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64ZeroRegisterStoresMoveRelaxedLowersSafely) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  auto* store32 = block->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 7, DataType::k32bit},
      Imm{0, DataType::k32bit});
  store32->output()->setDataType(DataType::k32bit);
  auto* store64 = block->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 19, DataType::k64bit},
      Imm{0, DataType::k64bit});
  store64->output()->setDataType(DataType::k64bit);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 40> bytes;
  bytes.fill(0xA5);
  func(bytes.data());
  for (size_t i = 0; i < bytes.size(); i++) {
    const bool should_be_zero = (i >= 7 && i < 11) || (i >= 19 && i < 27);
    EXPECT_EQ(bytes[i], should_be_zero ? 0 : 0xA5)
        << "unexpected byte at offset " << i;
  }

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?r\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?r\\s+xzr"})) << disasm;
  EXPECT_FALSE(std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64ZeroRegisterStoresExecuteFallbackShapes) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  struct StoreCase {
    PhyLocation temporary;
    int32_t offset;
    DataType materialization_type;
    DataType stored_value_type;
    DataType store_type;
    uint64_t value;
    size_t width;
  };
  constexpr StoreCase kStores[] = {
      {X10, 1, DataType::k64bit, DataType::k8bit, DataType::k8bit, 0, 1},
      {X10, 3, DataType::k64bit, DataType::k16bit, DataType::k16bit, 0, 2},
      {X10, 5, DataType::k32bit, DataType::k32bit, DataType::k8bit, 0, 1},
      {X10, 6, DataType::k32bit, DataType::k32bit, DataType::k16bit, 0, 2},
      {X10, 8, DataType::k64bit, DataType::k32bit, DataType::k32bit, 0, 4},
      {X11, 16, DataType::k32bit, DataType::k64bit, DataType::k64bit, 0, 8},
      {X12, 32, DataType::k8bit, DataType::k8bit, DataType::k8bit, 0x7b, 1},
      {X13,
       34,
       DataType::k16bit,
       DataType::k16bit,
       DataType::k16bit,
       0xa1b2,
       2},
      {X14,
       40,
       DataType::k32bit,
       DataType::k32bit,
       DataType::k32bit,
       0x11223344,
       4},
      {X15,
       48,
       DataType::k64bit,
       DataType::k64bit,
       DataType::k64bit,
       0x1122334455667788,
       8},
  };

  for (const auto& store_case : kStores) {
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{store_case.temporary, store_case.materialization_type},
        Imm{store_case.value, store_case.materialization_type});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], store_case.offset, store_case.store_type},
        PhyReg{store_case.temporary, store_case.stored_value_type});
    store->output()->setDataType(store_case.store_type);
    store->getInput(0)->setLastUse();
  }

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 64> bytes;
  bytes.fill(0xA5);
  func(bytes.data());

  std::array<uint8_t, 64> expected;
  expected.fill(0xA5);
  for (const auto& store_case : kStores) {
    for (size_t i = 0; i < store_case.width; i++) {
      expected[store_case.offset + i] =
          static_cast<uint8_t>(store_case.value >> (i * 8));
    }
  }
  EXPECT_EQ(bytes, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();

  // Every shape in this test is intentionally ineligible for the peephole:
  // mismatched widths or a non-zero value.
  EXPECT_FALSE(
      std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wzr|xzr)"}))
      << disasm;
  EXPECT_FALSE(std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64FloatingPointZerosPreserveBitPatterns) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  constexpr double kPositiveZero = 0.0;
  constexpr double kNegativeZero = -0.0;
  auto* positive_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 8, DataType::kDouble},
      PhyReg{FP_ARGUMENT_REGS[0], DataType::kDouble});
  positive_store->output()->setDataType(DataType::kDouble);
  auto* negative_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 24, DataType::kDouble},
      PhyReg{FP_ARGUMENT_REGS[1], DataType::kDouble});
  negative_store->output()->setDataType(DataType::kDouble);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*, double, double)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 40> bytes;
  bytes.fill(0xa5);
  func(bytes.data(), kPositiveZero, kNegativeZero);
  std::array<uint8_t, 40> expected;
  expected.fill(0xa5);
  std::memcpy(expected.data() + 8, &kPositiveZero, sizeof(kPositiveZero));
  std::memcpy(expected.data() + 24, &kNegativeZero, sizeof(kNegativeZero));
  EXPECT_EQ(bytes, expected);

  double stored_positive = 1.0;
  double stored_negative = 1.0;
  std::memcpy(&stored_positive, bytes.data() + 8, sizeof(stored_positive));
  std::memcpy(&stored_negative, bytes.data() + 24, sizeof(stored_negative));
  EXPECT_EQ(stored_positive, 0.0);
  EXPECT_FALSE(std::signbit(stored_positive));
  EXPECT_EQ(stored_negative, 0.0);
  EXPECT_TRUE(std::signbit(stored_negative));

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_FALSE(
      std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wzr|xzr|wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64AdjacentStoresFoldToStorePairEndToEnd) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  auto* lower_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 0, DataType::kObject},
      PhyReg{ARGUMENT_REGS[1], DataType::kObject});
  lower_store->output()->setDataType(DataType::kObject);
  auto* upper_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 8, DataType::kObject},
      PhyReg{ARGUMENT_REGS[2], DataType::k64bit});
  upper_store->output()->setDataType(DataType::kObject);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint64_t*, uint64_t, uint64_t)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  constexpr uint64_t kFirst = 0x1122334455667788;
  constexpr uint64_t kSecond = 0x99aabbccddeeff00;
  std::array<uint64_t, 4> values = {
      0xa5a5a5a5a5a5a5a5,
      0xa5a5a5a5a5a5a5a5,
      0xa5a5a5a5a5a5a5a5,
      0xa5a5a5a5a5a5a5a5,
  };
  func(values.data(), kFirst, kSecond);

  EXPECT_EQ(values[0], kFirst);
  EXPECT_EQ(values[1], kSecond);
  EXPECT_EQ(values[2], 0xa5a5a5a5a5a5a5a5);
  EXPECT_EQ(values[3], 0xa5a5a5a5a5a5a5a5);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stp\\s+x1,\\s*x2"}))
      << disasm;
}

TEST_F(
    BackendTest,
    AArch64StorePairExecutesAtEightByteButNotSixteenByteAlignedBase) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* lower_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 0, DataType::kObject},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit});
  lower_store->output()->setDataType(DataType::kObject);
  auto* upper_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 8, DataType::kObject},
      PhyReg{ARGUMENT_REGS[2], DataType::k64bit});
  upper_store->output()->setDataType(DataType::kObject);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*, uint64_t, uint64_t)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  alignas(16) std::array<uint8_t, 48> bytes;
  bytes.fill(0xa5);
  auto* base = bytes.data() + 8;
  ASSERT_EQ(reinterpret_cast<uintptr_t>(base) % 8, 0);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(base) % 16, 8);
  constexpr uint64_t kFirst = 0x1122334455667788;
  constexpr uint64_t kSecond = 0x99aabbccddeeff00;
  func(base, kFirst, kSecond);

  std::array<uint8_t, 48> expected;
  expected.fill(0xa5);
  std::memcpy(expected.data() + 8, &kFirst, sizeof(kFirst));
  std::memcpy(expected.data() + 16, &kSecond, sizeof(kSecond));
  EXPECT_EQ(bytes, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  const std::regex pair{"stp\\s+x1,\\s*x2"};
  EXPECT_EQ(
      std::distance(
          std::sregex_iterator(disasm.begin(), disasm.end(), pair),
          std::sregex_iterator()),
      1)
      << disasm;
}

TEST_F(BackendTest, AArch64AdjacentLoadsFoldToLoadPairEndToEnd) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{ARGUMENT_REGS[1], DataType::k64bit},
      Ind{ARGUMENT_REGS[0], 0, DataType::k64bit});
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{ARGUMENT_REGS[2], DataType::k64bit},
      Ind{ARGUMENT_REGS[0], 8, DataType::k64bit});
  block->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[2], DataType::k64bit});

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(const uint64_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  constexpr std::array<uint64_t, 4> values = {
      0x1122334455667788,
      0x0102030405060708,
      0xa5a5a5a5a5a5a5a5,
      0xa5a5a5a5a5a5a5a5,
  };
  EXPECT_EQ(func(values.data()), values[0] + values[1]);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  const std::regex pair{"ldp\\s+x1,\\s*x2"};
  EXPECT_EQ(
      std::distance(
          std::sregex_iterator(disasm.begin(), disasm.end(), pair),
          std::sregex_iterator()),
      1)
      << disasm;
}

TEST_F(BackendTest, AArch64MulAddAndMulSubExecuteAtBothGpWidths) {
  auto compile_op =
      [this](
          Instruction::Opcode opcode, DataType data_type, size_t* code_size) {
        auto lirfunc = std::make_unique<Function>();
        auto* block = lirfunc->allocateBasicBlock();
        block->allocateInstr(
            opcode,
            nullptr,
            OutPhyReg{ARGUMENT_REGS[0], data_type},
            PhyReg{ARGUMENT_REGS[0], data_type},
            PhyReg{ARGUMENT_REGS[1], data_type},
            PhyReg{ARGUMENT_REGS[2], data_type});
        return CompilePreAllocated(lirfunc.release(), 16, code_size);
      };

  size_t madd64_size = 0;
  auto madd64 = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(
      compile_op(Instruction::kMulAdd, DataType::k64bit, &madd64_size));
  ASSERT_NE(madd64, nullptr);
  EXPECT_EQ(madd64(7, 9, 11), 74);

  size_t msub64_size = 0;
  auto msub64 = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(
      compile_op(Instruction::kMulSub, DataType::k64bit, &msub64_size));
  ASSERT_NE(msub64, nullptr);
  EXPECT_EQ(msub64(7, 9, 100), 37);

  size_t madd32_size = 0;
  auto madd32 = reinterpret_cast<uint32_t (*)(uint32_t, uint32_t, uint32_t)>(
      compile_op(Instruction::kMulAdd, DataType::k32bit, &madd32_size));
  ASSERT_NE(madd32, nullptr);
  EXPECT_EQ(
      madd32(0xfffffff0U, 3U, 0x31U),
      static_cast<uint32_t>(0xfffffff0U * 3U + 0x31U));

  size_t msub32_size = 0;
  auto msub32 = reinterpret_cast<uint32_t (*)(uint32_t, uint32_t, uint32_t)>(
      compile_op(Instruction::kMulSub, DataType::k32bit, &msub32_size));
  ASSERT_NE(msub32, nullptr);
  EXPECT_EQ(
      msub32(0xfffffff0U, 3U, 0x31U),
      static_cast<uint32_t>(0x31U - 0xfffffff0U * 3U));

  auto disassemble = [](const void* func, size_t code_size) {
    std::ostringstream out;
    Disassembler dis{reinterpret_cast<const char*>(func), code_size};
    dis.setPrintAddr(false);
    dis.setPrintInstBytes(false);
    dis.disassembleAll(out);
    return out.str();
  };
  EXPECT_TRUE(std::regex_search(
      disassemble(reinterpret_cast<const void*>(madd64), madd64_size),
      std::regex{"madd\\s+x0,\\s*x0,\\s*x1,\\s*x2"}));
  EXPECT_TRUE(std::regex_search(
      disassemble(reinterpret_cast<const void*>(msub64), msub64_size),
      std::regex{"msub\\s+x0,\\s*x0,\\s*x1,\\s*x2"}));
  EXPECT_TRUE(std::regex_search(
      disassemble(reinterpret_cast<const void*>(madd32), madd32_size),
      std::regex{"madd\\s+w0,\\s*w0,\\s*w1,\\s*w2"}));
  EXPECT_TRUE(std::regex_search(
      disassemble(reinterpret_cast<const void*>(msub32), msub32_size),
      std::regex{"msub\\s+w0,\\s*w0,\\s*w1,\\s*w2"}));
}

TEST_F(
    BackendTest,
    AArch64MulArithmeticFusesThroughFullPipelineAndPreservesFlags) {
  struct CompiledOperation {
    void* address;
    size_t code_size;
  };

  auto compile_operation = [this](bool subtract) {
    auto lirfunc = std::make_unique<Function>();
    auto* entry = lirfunc->allocateBasicBlock();
    auto* true_block = lirfunc->allocateBasicBlock();
    auto* false_block = lirfunc->allocateBasicBlock();
    auto* epilogue = lirfunc->allocateBasicBlock();

    auto* lhs = entry->allocateInstr(
        Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{0});
    auto* rhs = entry->allocateInstr(
        Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{1});
    auto* accumulator = entry->allocateInstr(
        Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{2});
    auto* comparison = entry->allocateInstr(
        Instruction::kLessThanUnsigned,
        nullptr,
        OutVReg{DataType::k8bit},
        VReg{lhs},
        VReg{rhs});
    auto* product = entry->allocateInstr(
        Instruction::kMul,
        nullptr,
        OutVReg{DataType::k64bit},
        VReg{lhs},
        VReg{rhs});
    auto* result = entry->allocateInstr(
        subtract ? Instruction::kSub : Instruction::kAdd,
        nullptr,
        OutVReg{DataType::k64bit},
        subtract ? VReg{accumulator} : VReg{product},
        subtract ? VReg{product} : VReg{accumulator});
    entry->allocateInstr(Instruction::kCondBranch, nullptr, VReg{comparison});
    entry->addSuccessor(true_block);
    entry->addSuccessor(false_block);

    true_block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
        VReg{result});
    true_block->allocateInstr(Instruction::kReturn, nullptr);
    true_block->addSuccessor(epilogue);

    false_block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
        Imm{0, DataType::k64bit});
    false_block->allocateInstr(Instruction::kReturn, nullptr);
    false_block->addSuccessor(epilogue);

    size_t code_size = 0;
    void* address = CompileAfterTargetSelect(lirfunc.get(), &code_size);
    return CompiledOperation{address, code_size};
  };

  CompiledOperation madd = compile_operation(false);
  CompiledOperation msub = compile_operation(true);
  ASSERT_NE(madd.address, nullptr);
  ASSERT_NE(msub.address, nullptr);
  ASSERT_GT(madd.code_size, 0);
  ASSERT_GT(msub.code_size, 0);

  auto madd_func = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(
      madd.address);
  auto msub_func = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t)>(
      msub.address);

  // The compare is deliberately before the fused arithmetic.  Correct branch
  // results therefore prove that MADD/MSUB did not clobber NZCV.
  EXPECT_EQ(madd_func(2, 3, 5), 11);
  EXPECT_EQ(madd_func(4, 3, 5), 0);
  EXPECT_EQ(msub_func(2, 3, 20), 14);
  EXPECT_EQ(msub_func(4, 3, 20), 0);

  auto disassemble = [](const CompiledOperation& compiled) {
    std::ostringstream out;
    Disassembler dis{
        reinterpret_cast<const char*>(compiled.address), compiled.code_size};
    dis.setPrintAddr(false);
    dis.setPrintInstBytes(false);
    dis.disassembleAll(out);
    return out.str();
  };
  const std::string madd_disasm = disassemble(madd);
  const std::string msub_disasm = disassemble(msub);
  EXPECT_TRUE(std::regex_search(
      madd_disasm,
      std::regex{"madd\\s+x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+"}))
      << madd_disasm;
  EXPECT_TRUE(std::regex_search(
      msub_disasm,
      std::regex{"msub\\s+x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+"}))
      << msub_disasm;
  EXPECT_EQ(madd_disasm.find("mul "), std::string::npos) << madd_disasm;
  EXPECT_EQ(msub_disasm.find("mul "), std::string::npos) << msub_disasm;
}

TEST_F(BackendTest, AArch64SubSetFlagsPreservesSubtractionResult) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  block->allocateInstr(
      Instruction::kA64SubSetFlags,
      nullptr,
      OutPhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit});

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  EXPECT_EQ(func(3, 7), static_cast<uint64_t>(3 - 7));

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  EXPECT_TRUE(
      std::regex_search(out.str(), std::regex{"subs\\s+x0,\\s*x0,\\s*x1"}))
      << out.str();
}

TEST_F(
    BackendTest,
    AArch64PhysicalSubOutputFallsBackWhenItOverwritesComparedInput) {
  auto lirfunc = std::make_unique<Function>();
  auto* entry = lirfunc->allocateBasicBlock();
  auto* true_block = lirfunc->allocateBasicBlock();
  auto* false_block = lirfunc->allocateBasicBlock();
  auto* epilogue = lirfunc->allocateBasicBlock();

  entry->allocateInstr(
      Instruction::kSub,
      nullptr,
      OutPhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit});
  auto* comparison = entry->allocateInstr(
      Instruction::kLessThanUnsigned,
      nullptr,
      OutVReg{DataType::k8bit},
      PhyReg{ARGUMENT_REGS[0], DataType::k64bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k64bit});
  entry->allocateInstr(Instruction::kCondBranch, nullptr, VReg{comparison});
  entry->addSuccessor(true_block);
  entry->addSuccessor(false_block);

  true_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      Imm{1, DataType::k64bit});
  true_block->allocateInstr(Instruction::kReturn, nullptr);
  true_block->addSuccessor(epilogue);

  false_block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      Imm{0, DataType::k64bit});
  false_block->allocateInstr(Instruction::kReturn, nullptr);
  false_block->addSuccessor(epilogue);

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
      CompileAfterTargetSelect(lirfunc.get(), &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  // The compare must observe the already-written x0 value: 10 - 7 == 3.
  // An unsafe SUBS fusion would instead branch on the flags for 10 - 7.
  EXPECT_EQ(func(10, 7), 1);
  EXPECT_EQ(func(20, 7), 0);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"sub\\s+x0,\\s*x0,\\s*x1"}))
      << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"cmp\\s+x0,\\s*x1"}))
      << disasm;
  EXPECT_FALSE(
      std::regex_search(disasm, std::regex{"subs\\s+x0,\\s*x0,\\s*x1"}))
      << disasm;
}

TEST_F(BackendTest, AArch64AddedAddressWithOffsetFallsBackAndExecutes) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* epilogue = lirfunc->allocateBasicBlock();

  auto* base = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::kObject}, Imm{0});
  auto* index = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  auto* value = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{2});
  auto* address = block->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutVReg{DataType::kObject},
      VReg{base},
      VReg{index});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{address, 8, DataType::k64bit},
      VReg{value});
  store->output()->setDataType(DataType::k64bit);
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      VReg{value});
  block->allocateInstr(Instruction::kReturn, nullptr);
  block->addSuccessor(epilogue);

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(uint8_t*, uint64_t, uint64_t)>(
      CompileAfterTargetSelect(lirfunc.get(), &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  // A non-zero offset deliberately keeps the Add outside the folded
  // base-plus-index form.  Use a zero runtime index so this remains a short
  // executable check of the ordinary-register fallback without constructing
  // a physical XZR operand: XZR is not part of allocatable LIR and must only
  // be covered structurally at target selection.
  constexpr uint64_t kValue = 0x1122334455667788;
  std::array<uint8_t, 32> bytes;
  bytes.fill(0xa5);
  std::array<uint8_t, 32> expected = bytes;
  std::memcpy(expected.data() + 8, &kValue, sizeof(kValue));

  EXPECT_EQ(func(bytes.data(), 0, kValue), kValue);
  EXPECT_EQ(bytes, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(
      disasm, std::regex{"(^|\\n)\\s*add\\s+x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+"}))
      << disasm;
  EXPECT_TRUE(std::regex_search(
      disasm, std::regex{"str\\s+x[0-9]+,\\s*\\[x[0-9]+,\\s*#8\\]"}))
      << disasm;
}

TEST_F(BackendTest, AArch64AddedAddressesExecuteThroughFullPipeline) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* epilogue = lirfunc->allocateBasicBlock();

  auto* base = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::kObject}, Imm{0});
  auto* index = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  auto* value = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{2});

  auto* store_address = block->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutVReg{DataType::kObject},
      VReg{base},
      VReg{index});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{store_address, 0, DataType::k64bit},
      VReg{value});
  store->output()->setDataType(DataType::k64bit);

  auto* load_address = block->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutVReg{DataType::kObject},
      VReg{base},
      VReg{index});
  auto* loaded = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg{DataType::k64bit},
      Ind{load_address, 0, DataType::k64bit});
  static_cast<Operand*>(loaded->getInput(0))->setDataType(DataType::k64bit);
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      VReg{loaded});
  block->allocateInstr(Instruction::kReturn, nullptr);
  block->addSuccessor(epilogue);

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(uint8_t*, uint64_t, uint64_t)>(
      CompileAfterTargetSelect(lirfunc.get(), &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  constexpr size_t kOffset = 13;
  constexpr uint64_t kValue = 0x1122334455667788;
  std::array<uint8_t, 48> bytes;
  bytes.fill(0xa5);
  std::array<uint8_t, 48> expected = bytes;
  std::memcpy(expected.data() + kOffset, &kValue, sizeof(kValue));

  EXPECT_EQ(func(bytes.data(), kOffset, kValue), kValue);
  EXPECT_EQ(bytes, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(
      disasm, std::regex{"str\\s+x[0-9]+,\\s*\\[x[0-9]+,\\s*x[0-9]+\\]"}))
      << disasm;
  EXPECT_TRUE(std::regex_search(
      disasm, std::regex{"ldr\\s+x[0-9]+,\\s*\\[x[0-9]+,\\s*x[0-9]+\\]"}))
      << disasm;
  EXPECT_FALSE(std::regex_search(
      disasm, std::regex{"(^|\\n)\\s*add\\s+x[0-9]+,\\s*x[0-9]+,\\s*x[0-9]+"}))
      << disasm;
}

TEST_F(BackendTest, AArch64ShiftedIndexesExecuteThroughFullPipeline) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();
  auto* epilogue = lirfunc->allocateBasicBlock();

  auto* base = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::kObject}, Imm{0});
  auto* index = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{1});
  auto* value = block->allocateInstr(
      Instruction::kLoadArg, nullptr, OutVReg{DataType::k64bit}, Imm{2});

  auto* store_index = block->allocateInstr(
      Instruction::kLShift,
      nullptr,
      OutVReg{DataType::k64bit},
      VReg{index},
      Imm{3, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{base, store_index, DataType::k64bit},
      VReg{value});
  store->output()->setDataType(DataType::k64bit);

  auto* load_index = block->allocateInstr(
      Instruction::kLShift,
      nullptr,
      OutVReg{DataType::k64bit},
      VReg{index},
      Imm{3, DataType::k64bit});
  auto* loaded = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg{DataType::k64bit},
      Ind{base, load_index, DataType::k64bit});
  static_cast<Operand*>(loaded->getInput(0))->setDataType(DataType::k64bit);
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
      VReg{loaded});
  block->allocateInstr(Instruction::kReturn, nullptr);
  block->addSuccessor(epilogue);

  size_t code_size = 0;
  auto func = reinterpret_cast<uint64_t (*)(uint8_t*, uint64_t, uint64_t)>(
      CompileAfterTargetSelect(lirfunc.get(), &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  constexpr size_t kElement = 3;
  constexpr size_t kOffset = kElement * sizeof(uint64_t);
  constexpr uint64_t kValue = 0x8877665544332211;
  std::array<uint8_t, 64> bytes;
  bytes.fill(0xa5);
  std::array<uint8_t, 64> expected = bytes;
  std::memcpy(expected.data() + kOffset, &kValue, sizeof(kValue));

  EXPECT_EQ(func(bytes.data(), kElement, kValue), kValue);
  EXPECT_EQ(bytes, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  EXPECT_TRUE(std::regex_search(
      disasm,
      std::regex{"str\\s+x[0-9]+,\\s*\\[x[0-9]+,\\s*x[0-9]+,\\s*lsl #3\\]"}))
      << disasm;
  EXPECT_TRUE(std::regex_search(
      disasm,
      std::regex{"ldr\\s+x[0-9]+,\\s*\\[x[0-9]+,\\s*x[0-9]+,\\s*lsl #3\\]"}))
      << disasm;
  EXPECT_FALSE(std::regex_search(
      disasm, std::regex{"lsl\\s+x[0-9]+,\\s*x[0-9]+,\\s*#3"}))
      << disasm;
}

TEST_F(BackendTest, AArch64SubSetFlagsBranchesAtSignedUnsignedBoundaries) {
  struct CompiledComparison {
    void* address;
    size_t code_size;
  };

  auto compile_comparison = [this](
                                Instruction::Opcode compare_opcode,
                                DataType data_type) {
    auto lirfunc = std::make_unique<Function>();
    auto* entry = lirfunc->allocateBasicBlock();
    auto* true_block = lirfunc->allocateBasicBlock();
    auto* false_block = lirfunc->allocateBasicBlock();
    auto* epilogue = lirfunc->allocateBasicBlock();

    auto* lhs = entry->allocateInstr(
        Instruction::kLoadArg, nullptr, OutVReg{data_type}, Imm{0});
    auto* rhs = entry->allocateInstr(
        Instruction::kLoadArg, nullptr, OutVReg{data_type}, Imm{1});
    auto* difference = entry->allocateInstr(
        Instruction::kSub, nullptr, OutVReg{data_type}, VReg{lhs}, VReg{rhs});
    auto* comparison = entry->allocateInstr(
        compare_opcode,
        nullptr,
        OutVReg{DataType::k8bit},
        VReg{lhs},
        VReg{rhs});
    // Keep the subtraction result live without changing NZCV between the
    // selected SUBS and its branch.
    entry->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{ARGUMENT_REGS[3], data_type},
        VReg{difference});
    entry->allocateInstr(Instruction::kCondBranch, nullptr, VReg{comparison});
    entry->addSuccessor(true_block);
    entry->addSuccessor(false_block);

    true_block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
        Imm{1, DataType::k64bit});
    true_block->allocateInstr(Instruction::kReturn, nullptr);
    true_block->addSuccessor(epilogue);

    false_block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc, DataType::k64bit},
        Imm{0, DataType::k64bit});
    false_block->allocateInstr(Instruction::kReturn, nullptr);
    false_block->addSuccessor(epilogue);

    size_t code_size = 0;
    void* address = CompileAfterTargetSelect(lirfunc.get(), &code_size);
    return CompiledComparison{address, code_size};
  };

  auto signed32 =
      compile_comparison(Instruction::kLessThanEqualSigned, DataType::k32bit);
  auto unsigned32 =
      compile_comparison(Instruction::kLessThanEqualUnsigned, DataType::k32bit);
  auto signed64 =
      compile_comparison(Instruction::kLessThanEqualSigned, DataType::k64bit);
  auto unsigned64 =
      compile_comparison(Instruction::kLessThanEqualUnsigned, DataType::k64bit);

  ASSERT_NE(signed32.address, nullptr);
  ASSERT_NE(unsigned32.address, nullptr);
  ASSERT_NE(signed64.address, nullptr);
  ASSERT_NE(unsigned64.address, nullptr);
  ASSERT_GT(signed32.code_size, 0);
  ASSERT_GT(unsigned32.code_size, 0);
  ASSERT_GT(signed64.code_size, 0);
  ASSERT_GT(unsigned64.code_size, 0);

  auto signed32_func =
      reinterpret_cast<uint64_t (*)(uint32_t, uint32_t)>(signed32.address);
  auto unsigned32_func =
      reinterpret_cast<uint64_t (*)(uint32_t, uint32_t)>(unsigned32.address);
  auto signed64_func =
      reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(signed64.address);
  auto unsigned64_func =
      reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(unsigned64.address);

  // Signed cases exercise N xor V at the overflow boundaries and Z on equal.
  EXPECT_EQ(signed32_func(0x80000000U, 0x7fffffffU), 1);
  EXPECT_EQ(signed32_func(0x7fffffffU, 0x80000000U), 0);
  EXPECT_EQ(signed32_func(0x80000000U, 0x80000000U), 1);
  EXPECT_EQ(signed64_func(0x8000000000000000ULL, 0x7fffffffffffffffULL), 1);
  EXPECT_EQ(signed64_func(0x7fffffffffffffffULL, 0x8000000000000000ULL), 0);
  EXPECT_EQ(signed64_func(0x8000000000000000ULL, 0x8000000000000000ULL), 1);

  // Unsigned cases exercise carry/borrow and Z at zero and the maximum value.
  EXPECT_EQ(unsigned32_func(0, 0xffffffffU), 1);
  EXPECT_EQ(unsigned32_func(0xffffffffU, 0), 0);
  EXPECT_EQ(unsigned32_func(0xffffffffU, 0xffffffffU), 1);
  EXPECT_EQ(unsigned64_func(0, 0xffffffffffffffffULL), 1);
  EXPECT_EQ(unsigned64_func(0xffffffffffffffffULL, 0), 0);
  EXPECT_EQ(unsigned64_func(0xffffffffffffffffULL, 0xffffffffffffffffULL), 1);

  auto disassemble = [](const CompiledComparison& compiled) {
    std::ostringstream out;
    Disassembler dis{
        reinterpret_cast<const char*>(compiled.address), compiled.code_size};
    dis.setPrintAddr(false);
    dis.setPrintInstBytes(false);
    dis.disassembleAll(out);
    return out.str();
  };
  for (const CompiledComparison* compiled : {&signed32, &unsigned32}) {
    const std::string disasm = disassemble(*compiled);
    EXPECT_TRUE(std::regex_search(disasm, std::regex{"subs\\s+w[0-9]+,"}))
        << disasm;
    EXPECT_EQ(disasm.find("cmp "), std::string::npos) << disasm;
  }
  for (const CompiledComparison* compiled : {&signed64, &unsigned64}) {
    const std::string disasm = disassemble(*compiled);
    EXPECT_TRUE(std::regex_search(disasm, std::regex{"subs\\s+x[0-9]+,"}))
        << disasm;
    EXPECT_EQ(disasm.find("cmp "), std::string::npos) << disasm;
  }
}

TEST_F(BackendTest, AArch64ZeroRegisterStoresHandleDetailedAddressForms) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  constexpr size_t kBaseIndex = 6000;
  constexpr size_t kIndexedOffset = 6001;
  struct StoreCase {
    int32_t offset;
    DataType type;
    size_t width;
    size_t absolute_index;
  };
  // Cover an unscaled negative offset, negative and positive offsets that
  // require address synthesis, and deliberately unaligned destinations.
  // Canary checking verifies exact byte widths rather than relying on
  // naturally aligned happy paths.
  constexpr StoreCase kOffsetStores[] = {
      {-4099, DataType::k8bit, 1, kBaseIndex - 4099},
      {-257, DataType::k16bit, 2, kBaseIndex - 257},
      {-55, DataType::k32bit, 4, kBaseIndex - 55},
      {-47, DataType::k64bit, 8, kBaseIndex - 47},
      {-31, DataType::kObject, 8, kBaseIndex - 31},
      {4099, DataType::k64bit, 8, kBaseIndex + 4099},
  };

  for (const auto& store_case : kOffsetStores) {
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], store_case.offset, store_case.type},
        Imm{0, store_case.type});
    store->output()->setDataType(store_case.type);
  }

  auto* indexed_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{
          PhyLocation(ARGUMENT_REGS[0]),
          PhyLocation(ARGUMENT_REGS[1]),
          0,
          DataType::k32bit},
      Imm{0, DataType::k32bit});
  indexed_store->output()->setDataType(DataType::k32bit);

  // OutInd accepts the byte scale and converts it to the internal SIB-style
  // log2 multiplier used by MemoryIndirect.  A value of 2 therefore means a
  // 2-byte scale here and becomes multiplier 1 internally.
  constexpr unsigned int kScaledIndexedNumBytes = 2;
  constexpr int32_t kScaledIndexedExtraOffset = 257;
  constexpr size_t kScaledIndexedAbsolute = kBaseIndex +
      (kIndexedOffset * kScaledIndexedNumBytes) + kScaledIndexedExtraOffset;
  auto* scaled_indexed_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{
          PhyLocation(ARGUMENT_REGS[0]),
          PhyLocation(ARGUMENT_REGS[1]),
          kScaledIndexedNumBytes,
          kScaledIndexedExtraOffset,
          DataType::kObject},
      Imm{0, DataType::kObject});
  scaled_indexed_store->output()->setDataType(DataType::kObject);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint8_t*, uint64_t)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint8_t, 32768> bytes;
  bytes.fill(0xA5);
  func(bytes.data() + kBaseIndex, kIndexedOffset);

  std::array<bool, 32768> should_be_zero{};
  for (const auto& store_case : kOffsetStores) {
    for (size_t i = 0; i < store_case.width; i++) {
      should_be_zero[store_case.absolute_index + i] = true;
    }
  }
  for (size_t i = 0; i < 4; i++) {
    should_be_zero[kBaseIndex + kIndexedOffset + i] = true;
  }
  for (size_t i = 0; i < 8; i++) {
    should_be_zero[kScaledIndexedAbsolute + i] = true;
  }
  for (size_t i = 0; i < bytes.size(); i++) {
    EXPECT_EQ(bytes[i], should_be_zero[i] ? 0 : 0xA5)
        << "unexpected byte at offset " << i;
  }

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();

  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rb\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?rh\\s+wzr"})) << disasm;
  EXPECT_TRUE(std::regex_search(disasm, std::regex{"stu?r\\s+wzr"})) << disasm;
  const std::regex xzr_store{"stu?r\\s+xzr"};
  const auto xzr_store_count = std::distance(
      std::sregex_iterator(disasm.begin(), disasm.end(), xzr_store),
      std::sregex_iterator());
  EXPECT_GE(xzr_store_count, 4) << disasm;
  EXPECT_FALSE(std::regex_search(disasm, std::regex{"stu?r(b|h)?\\s+(wsp|sp)"}))
      << disasm;
}

TEST_F(BackendTest, AArch64ConstantFillStorePairExecutesEndToEnd) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  constexpr uint64_t kFill = 0x1122334455667788;
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{kFill, DataType::k64bit});
  auto* lower_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 0, DataType::kObject},
      PhyReg{X8, DataType::k64bit});
  lower_store->output()->setDataType(DataType::kObject);
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{kFill, DataType::k64bit});
  auto* upper_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{ARGUMENT_REGS[0], 8, DataType::kObject},
      PhyReg{X8, DataType::k64bit});
  upper_store->output()->setDataType(DataType::kObject);

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint64_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint64_t, 4> values;
  values.fill(0xa5a5a5a5a5a5a5a5);
  func(values.data());
  EXPECT_EQ(values[0], kFill);
  EXPECT_EQ(values[1], kFill);
  EXPECT_EQ(values[2], 0xa5a5a5a5a5a5a5a5);
  EXPECT_EQ(values[3], 0xa5a5a5a5a5a5a5a5);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  const std::regex fill_pair{"stp\\s+x8,\\s*x8"};
  EXPECT_EQ(
      std::distance(
          std::sregex_iterator(disasm.begin(), disasm.end(), fill_pair),
          std::sregex_iterator()),
      1)
      << disasm;
}

TEST_F(BackendTest, AArch64ConstantFillBitPatternsPreserveCanaries) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  struct FillCase {
    PhyLocation temporary;
    int32_t lower_offset;
    uint64_t value;
    DataType type;
  };
  constexpr FillCase kFills[] = {
      {X8, -16, 1, DataType::k64bit},
      {X9, 0, std::numeric_limits<uint64_t>::max(), DataType::kObject},
      {X10, 16, uint64_t{1} << 63, DataType::k64bit},
  };
  for (const auto& fill : kFills) {
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{fill.temporary, fill.type},
        Imm{fill.value, fill.type});
    auto* lower_store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], fill.lower_offset, fill.type},
        PhyReg{fill.temporary, fill.type});
    lower_store->output()->setDataType(fill.type);
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{fill.temporary, fill.type},
        Imm{fill.value, fill.type});
    auto* upper_store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], fill.lower_offset + 8, fill.type},
        PhyReg{fill.temporary, fill.type});
    upper_store->output()->setDataType(fill.type);
  }

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint64_t*)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  std::array<uint64_t, 8> values;
  values.fill(0xa5a5a5a5a5a5a5a5);
  func(values.data() + 2);
  for (size_t i = 0; i < std::size(kFills); i++) {
    EXPECT_EQ(values[i * 2], kFills[i].value);
    EXPECT_EQ(values[i * 2 + 1], kFills[i].value);
  }
  EXPECT_EQ(values[6], 0xa5a5a5a5a5a5a5a5);
  EXPECT_EQ(values[7], 0xa5a5a5a5a5a5a5a5);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  for (const char* pattern : {
           "stp\\s+x8,\\s*x8",
           "stp\\s+x9,\\s*x9",
           "stp\\s+x10,\\s*x10",
       }) {
    const std::regex pair{pattern};
    EXPECT_EQ(
        std::distance(
            std::sregex_iterator(disasm.begin(), disasm.end(), pair),
            std::sregex_iterator()),
        1)
        << disasm;
  }
}

TEST_F(BackendTest, AArch64StorePairOffsetBoundariesPreserveCanaries) {
  auto lirfunc = std::make_unique<Function>();
  auto* block = lirfunc->allocateBasicBlock();

  constexpr int32_t kLowerNegative = -512;
  constexpr int32_t kUpperNegative = -504;
  constexpr int32_t kLowerPositive = 496;
  constexpr int32_t kUpperPositive = 504;
  constexpr int32_t kRejectedLowerNegative = -528;
  constexpr int32_t kRejectedUpperNegative = -520;
  constexpr int32_t kRejectedLowerPositive = 512;
  constexpr int32_t kRejectedUpperPositive = 520;
  for (int32_t offset : {
           kRejectedLowerNegative,
           kRejectedUpperNegative,
           kLowerNegative,
           kUpperNegative,
           kLowerPositive,
           kUpperPositive,
           kRejectedLowerPositive,
           kRejectedUpperPositive,
       }) {
    const auto value_register =
        (offset == kRejectedLowerNegative || offset == kLowerNegative ||
         offset == kLowerPositive || offset == kRejectedLowerPositive)
        ? ARGUMENT_REGS[1]
        : ARGUMENT_REGS[2];
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{ARGUMENT_REGS[0], offset, DataType::kObject},
        PhyReg{value_register, DataType::k64bit});
    store->output()->setDataType(DataType::kObject);
  }

  size_t code_size = 0;
  auto func = reinterpret_cast<void (*)(uint64_t*, uint64_t, uint64_t)>(
      CompilePreAllocated(lirfunc.release(), 16, &code_size));
  ASSERT_NE(func, nullptr);
  ASSERT_GT(code_size, 0);

  constexpr uint64_t kFirst = 0x0123456789abcdef;
  constexpr uint64_t kSecond = 0xfedcba9876543210;
  std::array<uint64_t, 192> values;
  values.fill(0xa5a5a5a5a5a5a5a5);
  constexpr size_t kBaseElement = 96;
  func(values.data() + kBaseElement, kFirst, kSecond);

  std::array<uint64_t, 192> expected;
  expected.fill(0xa5a5a5a5a5a5a5a5);
  const auto elementAtOffset = [=](int32_t offset) {
    return static_cast<size_t>(static_cast<int64_t>(kBaseElement) + offset / 8);
  };
  expected[elementAtOffset(kLowerNegative)] = kFirst;
  expected[elementAtOffset(kUpperNegative)] = kSecond;
  expected[elementAtOffset(kLowerPositive)] = kFirst;
  expected[elementAtOffset(kUpperPositive)] = kSecond;
  expected[elementAtOffset(kRejectedLowerNegative)] = kFirst;
  expected[elementAtOffset(kRejectedUpperNegative)] = kSecond;
  expected[elementAtOffset(kRejectedLowerPositive)] = kFirst;
  expected[elementAtOffset(kRejectedUpperPositive)] = kSecond;
  EXPECT_EQ(values, expected);

  std::ostringstream out;
  Disassembler dis{reinterpret_cast<const char*>(func), code_size};
  dis.setPrintAddr(false);
  dis.setPrintInstBytes(false);
  dis.disassembleAll(out);
  const std::string disasm = out.str();
  const std::regex pair{"stp\\s+x1,\\s*x2"};
  EXPECT_EQ(
      std::distance(
          std::sregex_iterator(disasm.begin(), disasm.end(), pair),
          std::sregex_iterator()),
      2)
      << disasm;
}

#endif // CINDER_AARCH64

} // namespace jit::codegen
