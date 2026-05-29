// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/float_comparison_simplification.h"

#include "cinderx/Jit/hir/pass.h"

#include <memory>
#include <vector>

namespace jit::hir {

namespace {

// Map FloatCompare's CompareOp to the PrimitiveCompareOp that yields correct
// float comparison behavior on native fp compare instructions.
//
// For ordered comparisons we use the "Unsigned" variants: on x86 comisd only
// sets CF, so seta/setae/setb/setbe are correct while setg/setge/setl/setle
// are not.  On ARM fcmp sets both CF and the N/V flags, so both signed and
// unsigned work for normal numbers.
PrimitiveCompareOp mapToFloatPrimitiveCompareOp(CompareOp op) {
  switch (op) {
    case CompareOp::kLessThan:
      return PrimitiveCompareOp::kLessThanUnsigned;
    case CompareOp::kLessThanEqual:
      return PrimitiveCompareOp::kLessThanEqualUnsigned;
    case CompareOp::kEqual:
      return PrimitiveCompareOp::kEqual;
    case CompareOp::kNotEqual:
      return PrimitiveCompareOp::kNotEqual;
    case CompareOp::kGreaterThan:
      return PrimitiveCompareOp::kGreaterThanUnsigned;
    case CompareOp::kGreaterThanEqual:
      return PrimitiveCompareOp::kGreaterThanEqualUnsigned;
    default:
      JIT_ABORT("Unsupported CompareOp for float comparison: {}",
                static_cast<int>(op));
  }
}

// Check whether the given register has any real user (non-Decref/XDecref)
// other than the excluded instruction.  Recursively traces through Assign
// instructions.  Uses the pre-built reg_uses map for function-wide coverage.
bool hasOtherRealUsers(
    Register* reg,
    const Instr* excluded,
    const RegUses& reg_uses) {
  auto it = reg_uses.find(reg);
  if (it == reg_uses.end()) {
    return false;
  }
  for (Instr* user : it->second) {
    if (user->IsDecref() || user->IsXDecref()) {
      continue;
    }
    if (user == excluded) {
      continue;
    }
    if (user->IsAssign()) {
      if (hasOtherRealUsers(user->output(), excluded, reg_uses)) {
        return true;
      }
      continue;
    }
    return true;
  }
  return false;
}

// Remove Decref/XDecref instructions in the given block that reference the
// given register.  Safe to call after the main iteration loop.
void removeRefcountUsers(
    Register* reg,
    BasicBlock& block,
    std::vector<std::unique_ptr<Instr>>& removed) {
  for (auto it = block.begin(); it != block.end();) {
    Instr& instr = *it;
    ++it;
    if (!instr.IsDecref() && !instr.IsXDecref()) {
      continue;
    }
    for (std::size_t i = 0; i < instr.NumOperands(); ++i) {
      if (instr.GetOperand(i) == reg) {
        instr.unlink();
        removed.emplace_back(&instr);
        break;
      }
    }
  }
}

} // namespace

void FloatComparisonSimplification::Run(Function& irfunc) {
  RegUses reg_uses = collectDirectRegUses(irfunc);
  bool changed = false;
  std::vector<std::unique_ptr<Instr>> removed_instrs;
  std::vector<Register*> refcount_cleanup;

  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      Instr& instr = *it;
      ++it; // Advance before potentially unlinking instr.

      if (!instr.IsPrimitiveCompare()) {
        continue;
      }
      auto* pc = static_cast<PrimitiveCompare*>(&instr);
      if (pc->op() != PrimitiveCompareOp::kEqual &&
          pc->op() != PrimitiveCompareOp::kNotEqual) {
        continue;
      }

      Register* left = chaseAssignOperand(pc->GetOperand(0));
      Register* right = chaseAssignOperand(pc->GetOperand(1));

      Instr* left_def = left->instr();
      Instr* right_def = right->instr();

      if (!left_def->IsFloatCompare() || !right_def->IsLoadConst()) {
        continue;
      }

      Type right_type = right->type();
      if (!right_type.isSingleValue()) {
        continue;
      }
      PyObject* const_obj = right_type.asObject();
      bool compare_to_true = (const_obj == Py_True);
      bool compare_to_false = (const_obj == Py_False);
      if (!compare_to_true && !compare_to_false) {
        continue;
      }

      // NaN safety: only handle non-inverting cases.
      //   Equal  + Py_True  → identity    (no inversion)  ✓
      //   NotEqual + Py_False → identity  (no inversion)  ✓
      //   Equal  + Py_False → would invert (skip)
      //   NotEqual + Py_True  → would invert (skip)
      //
      // Boolean negation (not (a > b)) is NOT equivalent to operator
      // inversion (a <= b) when NaN is involved.  Under IEEE 754:
      //   NaN > x   = False   → not(NaN > x)   = True
      //   NaN <= x  = False   (ordered comparison, NaN → False)
      bool invert =
          (pc->op() == PrimitiveCompareOp::kEqual && compare_to_false) ||
          (pc->op() == PrimitiveCompareOp::kNotEqual && compare_to_true);
      if (invert) {
        continue;
      }

      auto* fc = static_cast<FloatCompare*>(left_def);

      // Defensive: FloatCompare guarantees its operands are FloatExact.
      JIT_DCHECK(
          fc->left()->type() <= TFloatExact,
          "FloatCompare left operand must be FloatExact");
      JIT_DCHECK(
          fc->right()->type() <= TFloatExact,
          "FloatCompare right operand must be FloatExact");

      // The FloatCompare must be in the same block.
      if (fc->block() != &block) {
        continue;
      }

      // Function-wide liveness: the FloatCompare output must have no other
      // real (non-Decref/XDecref) users besides this PrimitiveCompare.
      // Uses collectDirectRegUses to cover cross-block uses as well.
      if (hasOtherRealUsers(fc->output(), &instr, reg_uses)) {
        continue;
      }

      PrimitiveCompareOp new_op =
          mapToFloatPrimitiveCompareOp(fc->op());

      // Allocate registers and build the replacement sequence.
      // Reuse instr's output register so that downstream uses (CondBranch,
      // etc.) see the new instruction's result without register replacement.
      Register* unbox_left_reg = irfunc.env.AllocateRegister();
      Register* unbox_right_reg = irfunc.env.AllocateRegister();

      auto* unbox_left =
          PrimitiveUnbox::create(unbox_left_reg, fc->left(), TCDouble);
      auto* unbox_right =
          PrimitiveUnbox::create(unbox_right_reg, fc->right(), TCDouble);
      auto* new_cmp = PrimitiveCompare::create(
          instr.output(), new_op, unbox_left_reg, unbox_right_reg);

      unbox_left->copyBytecodeOffset(*fc);
      unbox_right->copyBytecodeOffset(*fc);
      new_cmp->copyBytecodeOffset(instr);

      // Insert unbox instructions before the old PrimitiveCompare, then
      // ReplaceWith handles inserting new_cmp and unlinking instr.
      unbox_left->InsertBefore(instr);
      unbox_right->InsertBefore(instr);
      instr.ReplaceWith(*new_cmp);

      // Unlink and schedule removal of the FloatCompare.  instr was
      // already unlinked by ReplaceWith.  LoadConst (right_def) is
      // intentionally NOT deleted — it may be shared via CSE and DCE
      // will clean it up if unused.
      fc->unlink();
      removed_instrs.emplace_back(fc);
      removed_instrs.emplace_back(&instr);

      // Defer Decref/XDecref cleanup to after the iteration loop to
      // avoid iterator invalidation (the outer iterator may be pointing
      // at a Decref that we would otherwise unlink).
      refcount_cleanup.push_back(fc->output());

      changed = true;
    }
  }

  // Deferred refcount cleanup — safe because the outer loop has finished.
  for (Register* reg : refcount_cleanup) {
    for (auto& block : irfunc.cfg.blocks) {
      removeRefcountUsers(reg, block, removed_instrs);
    }
  }

  // removed_instrs destructor frees all unlinked instructions.

  if (changed) {
    reflowTypes(irfunc);
  }
}

} // namespace jit::hir
