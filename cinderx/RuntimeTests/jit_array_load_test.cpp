// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"

#include "cinderx/Jit/hir/hir.h"

using ArrayLoadTest = RuntimeTest;

// Test that BINARY_SUBSCR on array('d') generates LoadArrayItem(TCDouble) in
// HIR.
TEST_F(ArrayLoadTest, BinarySubscrArrayDoubleGeneratesLoadArrayItem) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def load_array_double(a, i):
    return a[i]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

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
def load_array_double(a, i):
    return a[i]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

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

// Test that BINARY_SUBSCR generates both fast path (LoadArrayItem) and slow
// path (BinaryOp) for generic subscript.
TEST_F(ArrayLoadTest, BinarySubscrGeneratesBothPaths) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
def load_any(a, i):
    return a[i]
)",
      "load_any",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

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

  EXPECT_TRUE(found_load_array_item)
      << "Expected LoadArrayItem(TCDouble) (fast path) in HIR";
  EXPECT_TRUE(found_binary_op_subscr)
      << "Expected BinaryOp(kSubscript) (slow path) in HIR";
}

// Test that CondBranchCheckType guard is present for array type check.
TEST_F(ArrayLoadTest, CondBranchCheckTypeGuardPresent) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def load_array_double(a, i):
    return a[i]
)",
      "load_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

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
