// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace jit::lir {

class LIRTargetSelectTest : public RuntimeTest {
 public:
  std::string getSelectedLIRString(PyObject* func_obj) {
    JIT_CHECK(
        PyFunction_Check(func_obj),
        "Trying to compile something that isn't a function");
    BorrowedRef<PyFunctionObject> func{func_obj};

    PyObject* globals = PyFunction_GetGlobals(func_obj);
    if (!PyDict_CheckExact(globals) ||
        !PyDict_CheckExact(func->func_builtins)) {
      return "";
    }

    std::unique_ptr<hir::Function> irfunc(buildHIR(func));
    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);

    codegen::Environ env;
    env.ctx = getContext();

    CodeRuntime runtime{func};
    Ref<> reifier;
    if (irfunc->reifier != nullptr) {
      runtime.setReifier(irfunc->reifier);
    } else {
      reifier = makeFrameReifier(func->func_code);
      runtime.setReifier(reifier);
    }
    env.code_rt = &runtime;

    LIRGenerator lir_gen(irfunc.get(), &env);
    std::unique_ptr<Function> lir_func = lir_gen.TranslateFunction();
    selectTargetOpcodes(lir_func.get());

    std::stringstream ss;
    lir_func->sortBasicBlocks();
    ss << *lir_func << '\n';
    return ss.str();
  }
};

TEST(LIRTargetSelectOperandTest, ReleasesMemoryIndirectRegisterOperands) {
  auto base = codegen::ARGUMENT_REGS[3];
  auto index = codegen::ARGUMENT_REGS[2];

  MemoryIndirect memory(nullptr);
  memory.setMemoryIndirect(base.loc, index.loc, 2, 0x40);

  std::unique_ptr<OperandBase> released_base = memory.releaseBaseRegOperand();
  std::unique_ptr<OperandBase> released_index = memory.releaseIndexRegOperand();

  ASSERT_NE(released_base, nullptr);
  ASSERT_NE(released_index, nullptr);
  EXPECT_EQ(released_base->getPhyRegister(), base);
  EXPECT_EQ(released_index->getPhyRegister(), index);
  EXPECT_EQ(memory.getBaseRegOperand(), nullptr);
  EXPECT_EQ(memory.getIndexRegOperand(), nullptr);
}

#if defined(CINDER_AARCH64)
static std::string runTargetSelect(const char* lir_input_str) {
  std::unique_ptr<Function> func = Parser().parse(lir_input_str);
  selectTargetOpcodes(func.get());
  return fmt::format("{}", *func);
}

static std::vector<Instruction*> collectTargetSelectInstrs(BasicBlock& block) {
  std::vector<Instruction*> result;
  for (auto& instr : block.instructions()) {
    result.push_back(instr.get());
  }
  return result;
}

TEST_F(LIRTargetSelectTest, LegalizesSelectStackInputs) {
  Function func;
  BasicBlock* block = func.allocateBasicBlock();
  Instruction* select = block->allocateInstr(
      Instruction::kSelect,
      nullptr,
      OutVReg{DataType::k64bit},
      Stk{PhyLocation{-8, 8}, DataType::k8bit},
      Stk{PhyLocation{-16, 64}, DataType::k64bit},
      Stk{PhyLocation{-24, 64}, DataType::k64bit});

  selectTargetOpcodes(&func);

  std::vector<Instruction*> instrs = collectTargetSelectInstrs(*block);
  ASSERT_EQ(instrs.size(), 4);
  for (size_t i = 0; i < 3; i++) {
    ASSERT_TRUE(select->getInput(i)->isLinked());
    Instruction* input =
        static_cast<LinkedOperand*>(select->getInput(i))->getLinkedInstr();
    EXPECT_TRUE(input->isMove());
    EXPECT_TRUE(input->getInput(0)->isStack());
  }
}

TEST_F(LIRTargetSelectTest, LegalizesStackIncDecInputs) {
  for (Instruction::Opcode opcode : {Instruction::kInc, Instruction::kDec}) {
    SCOPED_TRACE(static_cast<int>(opcode));
    Function func;
    BasicBlock* block = func.allocateBasicBlock();
    PhyLocation stack_loc{-24, 64};
    block->allocateInstr(opcode, nullptr, Stk{stack_loc, DataType::k64bit});

    selectTargetOpcodes(&func);

    std::vector<Instruction*> instrs = collectTargetSelectInstrs(*block);
    ASSERT_EQ(instrs.size(), 3);
    EXPECT_TRUE(instrs[0]->isMove());
    EXPECT_TRUE(instrs[0]->getInput(0)->isStack());
    EXPECT_EQ(instrs[1]->opcode(), opcode);
    ASSERT_TRUE(instrs[1]->getInput(0)->isLinked());
    EXPECT_EQ(
        static_cast<LinkedOperand*>(instrs[1]->getInput(0))->getLinkedInstr(),
        instrs[0]);
    EXPECT_TRUE(instrs[2]->isMove());
    EXPECT_TRUE(instrs[2]->output()->isStack());
    ASSERT_TRUE(instrs[2]->getInput(0)->isLinked());
    EXPECT_EQ(
        static_cast<LinkedOperand*>(instrs[2]->getInput(0))->getLinkedInstr(),
        instrs[0]);
  }
}

TEST_F(LIRTargetSelectTest, LegalizesUnaryStackInputs) {
  for (Instruction::Opcode opcode :
       {Instruction::kNegate, Instruction::kInvert}) {
    SCOPED_TRACE(static_cast<int>(opcode));
    Function func;
    BasicBlock* block = func.allocateBasicBlock();
    block->allocateInstr(
        opcode,
        nullptr,
        OutVReg{DataType::k64bit},
        Stk{PhyLocation{-32, 64}, DataType::k64bit});

    selectTargetOpcodes(&func);

    std::vector<Instruction*> instrs = collectTargetSelectInstrs(*block);
    ASSERT_EQ(instrs.size(), 2);
    EXPECT_TRUE(instrs[0]->isMove());
    EXPECT_TRUE(instrs[0]->getInput(0)->isStack());
    EXPECT_EQ(instrs[1]->opcode(), opcode);
    ASSERT_TRUE(instrs[1]->getInput(0)->isLinked());
    EXPECT_EQ(
        static_cast<LinkedOperand*>(instrs[1]->getInput(0))->getLinkedInstr(),
        instrs[0]);
  }
}

TEST_F(LIRTargetSelectTest, LegalizesSignedSubWordInputs) {
  for (Instruction::Opcode opcode :
       {Instruction::kLessThanSigned, Instruction::kDiv}) {
    SCOPED_TRACE(static_cast<int>(opcode));
    Function func;
    BasicBlock* block = func.allocateBasicBlock();
    Instruction* lhs = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg{DataType::k8bit},
        Imm{0xff, DataType::k8bit});
    Instruction* rhs = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg{DataType::k16bit},
        Imm{1, DataType::k16bit});
    Instruction* op = block->allocateInstr(
        opcode, nullptr, OutVReg{DataType::k8bit}, VReg{lhs}, VReg{rhs});

    selectTargetOpcodes(&func);

    for (size_t i = 0; i < 2; i++) {
      ASSERT_TRUE(op->getInput(i)->isLinked());
      Instruction* input =
          static_cast<LinkedOperand*>(op->getInput(i))->getLinkedInstr();
      EXPECT_EQ(input->opcode(), Instruction::kSext);
      EXPECT_EQ(input->output()->dataType(), DataType::k32bit);
    }
  }
}

TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplier) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:64bit = Lea [%1:64bit + %2:64bit * 16 + 0x8]
  Return %3
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("= Move 16(0x10):64bit"), std::string::npos)
      << lir_str;
  EXPECT_NE(lir_str.find("= MulAdd %2:64bit"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("= Add "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("%3:64bit = Move %"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Lea "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, SelectsMulAddForLeaLargeMultiplierWithLargeOffset) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:64bit = Lea [%1:64bit + %2:64bit * 16 + 0x100001]
  Return %3
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("= Move 16(0x10):64bit"), std::string::npos)
      << lir_str;
  EXPECT_NE(lir_str.find("= Move 1048577(0x100001):64bit"), std::string::npos)
      << lir_str;
  EXPECT_NE(lir_str.find("= MulAdd %2:64bit"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("= Add "), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Lea "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, LegalizesComparisonOutputToMin32Bit) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  Return %3
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("%3:32bit = Equal "), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("%3:8bit = Equal "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, LegalizesBitwiseOutputToMin32Bit) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:8bit = Move 1
  %2:8bit = Move 2
  %3:8bit = And %1, %2
  Return %3
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("%3:32bit = And "), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("%3:8bit = And "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, SelectsBranchCCForSingleUseCompare) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
                   Cmp %1:64bit, %2:64bit
                   BranchE

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(
    LIRTargetSelectTest,
    SelectsBranchCCWhenInterveningInstructionsPreserveFlags) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  %4:64bit = Move 3
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
                   Cmp %1:64bit, %2:64bit
        %4:64bit = Move 3(0x3):64bit
                   BranchE

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(LIRTargetSelectTest, DoesNotSelectBranchCCAcrossFlagClobber) {
  const char* lir_input_str = R"(Function:
BB %0 - succs: %1 %2
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = Equal %1, %2
  %4:64bit = Add %1, %2
  CondBranch %3, BB%1, BB%2
BB %1 - preds: %0
  Return %1
BB %2 - preds: %0
  Return %2
)";

  const char* expected_lir_str = R"(Function:
BB %0 - succs: %1 %2
        %1:64bit = Move 1(0x1):64bit
        %2:64bit = Move 2(0x2):64bit
        %3:32bit = Equal %1:64bit, %2:64bit
        %4:64bit = Add %1:64bit, %2:64bit
                   CondBranch %3:32bit, BB%1, BB%2

BB %1 - preds: %0
                   Return %1:64bit

BB %2 - preds: %0
                   Return %2:64bit

)";

  EXPECT_EQ(runTargetSelect(lir_input_str), expected_lir_str);
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCForSingleUseCompareGuard) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  Guard 4, 0, %3, 0
  Return %1
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("LessThanUnsigned"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Guard "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, LegalizesGuardFPInputToGPInput) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:Double = Move 1
  Guard 4, 0, %1, 0
  Return %1
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find(":64bit = Move %1:Double"), std::string::npos)
      << lir_str;
  EXPECT_TRUE(std::regex_search(
      lir_str,
      std::regex{R"(Guard 4\(0x4\):64bit, 0\(0x0\):64bit, %\d+:64bit)"}))
      << lir_str;
  EXPECT_EQ(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, SelectsA64GuardCCThroughFlagPreservingInstrs) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1
  %2:64bit = Move 2
  %3:8bit = LessThanUnsigned %1, %2
  %4:64bit = Move 8
  Guard 4, 0, %3, 0
  Return %1
)";

  std::string lir_str = runTargetSelect(lir_input_str);

  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("LessThanUnsigned"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find("Guard "), std::string::npos) << lir_str;
}

TEST_F(LIRTargetSelectTest, A64GuardCCDeoptsThroughNearExit) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(1000000)

def test(a):
    return a ** 2

for _ in range(20000):
    assert test(3.0) == 9.0
)");

  Ref<PyFunctionObject> pyfunc(getGlobal("test"));
  ASSERT_NE(pyfunc, nullptr);

  std::string lir_str =
      getSelectedLIRString(reinterpret_cast<PyObject*>(pyfunc.get()));
  ASSERT_NE(lir_str.find("A64GuardCC"), std::string::npos) << lir_str;

  ASSERT_EQ(jit::compileFunction(pyfunc), jit::Result::OK);

  size_t deopts = 0;
  auto* ctx = jit::getContext();
  ctx->setGuardFailureCallback([&deopts](const DeoptMetadata& meta) {
    EXPECT_EQ(meta.reason, DeoptReason::kGuardFailure);
    deopts++;
  });

  auto value = Ref<>::steal(PyFloat_FromDouble(3.0));
  auto float_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(pyfunc.get()), value.get(), nullptr));
  ASSERT_NE(float_result, nullptr);
  ASSERT_TRUE(PyFloat_Check(float_result));
  EXPECT_EQ(PyFloat_AsDouble(float_result), 9.0);
  EXPECT_EQ(deopts, 0);

  auto infinity =
      Ref<>::steal(PyFloat_FromDouble(std::numeric_limits<double>::infinity()));
  auto overflow_result = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(pyfunc.get()), infinity.get(), nullptr));
  ctx->clearGuardFailureCallback();

  ASSERT_NE(overflow_result, nullptr);
  ASSERT_TRUE(PyFloat_Check(overflow_result));
  EXPECT_EQ(
      PyFloat_AsDouble(overflow_result),
      std::numeric_limits<double>::infinity());
  EXPECT_GE(deopts, 1);
}

TEST_F(LIRTargetSelectTest, KeepsPythonCompareBranchWhenResultHasExtraUses) {
  const char* src = R"(
def func(x, y):
  if x in y:
    return x
  return y
)";

  Ref<PyObject> pyfunc(compileAndGet(src, "func"));
  ASSERT_NE(pyfunc.get(), nullptr) << "Failed compiling func";

  std::string lir_str = getSelectedLIRString(pyfunc.get());

  // Compare<In> now produces an immortal bool, so the truth-value branch can
  // be selected as Cmp + BranchE while the Python compare result is still kept
  // for the guard.
  EXPECT_NE(lir_str.find("Compare<In>"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("PrimitiveCompare<Equal>"), std::string::npos)
      << lir_str;
  EXPECT_NE(lir_str.find("Guard 4"), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("Cmp "), std::string::npos) << lir_str;
  EXPECT_NE(lir_str.find("BranchE"), std::string::npos) << lir_str;
  EXPECT_EQ(lir_str.find(" = Equal "), std::string::npos) << lir_str;
}
#endif

} // namespace jit::lir
