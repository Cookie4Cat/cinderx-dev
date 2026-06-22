// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"

using ArrayLoadTest = RuntimeTest;

namespace {
// The array.array('d') subscript fast path is emitted by the Simplify pass, so
// run SSA construction + Simplify before inspecting the HIR.
void runArrayFastPath(std::unique_ptr<jit::hir::Function>& irfunc) {
  jit::hir::SSAify{}.Run(*irfunc);
  jit::hir::Simplify{}.Run(*irfunc);
}

size_t countDoubleArrayLoads(const jit::hir::Function& irfunc) {
  size_t count = 0;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsLoadArrayItem()) {
        auto* lai = static_cast<const jit::hir::LoadArrayItem*>(&instr);
        if (lai->type() <= jit::hir::TCDouble) {
          ++count;
        }
      }
    }
  }
  return count;
}

size_t countBinarySubscrs(const jit::hir::Function& irfunc) {
  size_t count = 0;
  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsBinaryOp()) {
        auto* binop = static_cast<const jit::hir::BinaryOp*>(&instr);
        if (binop->op() == jit::hir::BinaryOpKind::kSubscript) {
          ++count;
        }
      }
    }
  }
  return count;
}
} // namespace

// Test that BINARY_SUBSCR on array('d') with a known index shape generates
// LoadArrayItem(TCDouble) in HIR.
TEST_F(ArrayLoadTest, BinarySubscrArrayDoubleGeneratesLoadArrayItem) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def load_array_double(a):
    return a[0]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  runArrayFastPath(irfunc);

  bool found_load_array_item = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsLoadArrayItem()) {
        auto* lai = static_cast<const jit::hir::LoadArrayItem*>(&instr);
        if (lai->type() <= jit::hir::TCDouble) {
          found_load_array_item = true;
        }
      }
    }
  }

  EXPECT_TRUE(found_load_array_item)
      << "Expected LoadArrayItem(TCDouble) in HIR for array('d') load";
}

// Test that PrimitiveBox(CDouble) is present in the same CFG as
// LoadArrayItem(TCDouble).
TEST_F(ArrayLoadTest, LoadArrayItemCoexistsWithPrimitiveBox) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def load_array_double(a):
    return a[0]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  runArrayFastPath(irfunc);

  bool found_load_array_item = false;
  bool found_primitive_box_cdouble = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsLoadArrayItem()) {
        auto* lai = static_cast<const jit::hir::LoadArrayItem*>(&instr);
        if (lai->type() <= jit::hir::TCDouble) {
          found_load_array_item = true;
        }
      }
      if (instr.IsPrimitiveBox()) {
        auto* box = static_cast<const jit::hir::PrimitiveBox*>(&instr);
        if (box->type() <= jit::hir::TCDouble) {
          found_primitive_box_cdouble = true;
        }
      }
    }
  }

  EXPECT_TRUE(found_load_array_item)
      << "Expected LoadArrayItem(TCDouble) in HIR";
  EXPECT_TRUE(found_primitive_box_cdouble)
      << "Expected PrimitiveBox(CDouble) in HIR alongside LoadArrayItem";
}

// Test that BINARY_SUBSCR with unknown container/index shapes stays generic.
TEST_F(ArrayLoadTest, BinarySubscrUnknownShapeGeneratesGenericPath) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
def load_any(a, i):
    return a[i]
)",
      "load_any",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  runArrayFastPath(irfunc);

  bool found_load_array_item = false;
  bool found_binary_op_subscr = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsLoadArrayItem()) {
        auto* lai = static_cast<const jit::hir::LoadArrayItem*>(&instr);
        if (lai->type() <= jit::hir::TCDouble) {
          found_load_array_item = true;
        }
      }
      if (instr.IsBinaryOp()) {
        auto* binop = static_cast<const jit::hir::BinaryOp*>(&instr);
        if (binop->op() == jit::hir::BinaryOpKind::kSubscript) {
          found_binary_op_subscr = true;
        }
      }
    }
  }

  EXPECT_FALSE(found_load_array_item)
      << "Did not expect LoadArrayItem for unknown load shapes";
  EXPECT_TRUE(found_binary_op_subscr) << "Expected BinaryOp(kSubscript) in HIR";
}

// Test that CondBranchCheckType guard is present for array type check when the
// fast path is enabled.
TEST_F(ArrayLoadTest, CondBranchCheckTypeGuardPresent) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def load_array_double(a):
    return a[0]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  runArrayFastPath(irfunc);

  bool found_cond_branch_check = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.opcode() == jit::hir::Opcode::kCondBranchCheckType) {
        found_cond_branch_check = true;
      }
    }
  }

  EXPECT_TRUE(found_cond_branch_check)
      << "Expected CondBranchCheckType guard for array type check";
}

TEST_F(ArrayLoadTest, SlowPathIsNotReSpecializedAcrossSimplifyPasses) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
def load_any(a):
    return a[0]
)",
      "load_any",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  jit::Compiler::runPasses(*irfunc, jit::PassConfig::kSimplify);

  std::string hir = jit::hir::HIRPrinter{}.ToString(*irfunc);
  EXPECT_EQ(countDoubleArrayLoads(*irfunc), 1) << hir;
  EXPECT_EQ(countBinarySubscrs(*irfunc), 1) << hir;
}
