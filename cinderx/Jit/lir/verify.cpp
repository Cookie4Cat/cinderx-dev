// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/printer.h"

namespace jit::lir {

bool verifyPostRegAllocInvariants(Function* func, std::ostream& err) {
  auto& blocks = func->basicblocks();
  for (auto iter = blocks.begin(); iter != blocks.end();) {
    auto& block = *iter;
    ++iter;
    auto& succs = block->successors();
    BasicBlock* next_block = iter == blocks.end() ? nullptr : *iter;
    std::unordered_set<BasicBlock*> branched_blocks;
    for (auto& instr : block->instructions()) {
      if (instr->isBranch() || instr->isBranchCC()) {
        auto num_inputs = instr->getNumInputs();
        JIT_DCHECK(num_inputs > 0, "Branch must have at least one input.");
        if (num_inputs == 2) {
#if defined(CINDER_AARCH64)
          auto reg_input = instr->getInput(0);
          auto reg_type = reg_input->dataType();
          JIT_DCHECK(
              (instr->opcode() == Instruction::kBranchZ ||
               instr->opcode() == Instruction::kBranchNZ) &&
                  reg_input->isReg() &&
                  (reg_type == OperandBase::k32bit ||
                   reg_type == OperandBase::k64bit ||
                   reg_type == OperandBase::kObject),
              "Two-input branches must be AArch64 register-tested "
              "BranchZ/BranchNZ.");
#else
          JIT_DCHECK(false, "Two-input branches are only valid on AArch64.");
#endif
        } else {
          JIT_DCHECK(
              num_inputs == 1, "Branch must have one or two inputs.");
        }
        auto operand = instr->getInput(num_inputs - 1);
        JIT_DCHECK(
            operand->type() == OperandBase::kLabel,
            "Branch must jump to a label.");
        branched_blocks.insert(operand->getBasicBlock());
      }
    }

    for (const auto& succ : succs) {
      // Go through the instructions and ensure that each successor has a
      // matching jump.
      if (succ == next_block && next_block->section() == block->section()) {
        // If a successor is physically the next block in the block order and
        // the blocks are emitted to the same section, we don't need a branch.
        continue;
      }
      // Ensure that a jump to the successor exists.
      if (!branched_blocks.contains(succ)) {
        fmt::print(
            err,
            "ERROR: Basic block {} does not contain a jump to non-immediate "
            "successor {}.\n",
            block->id(),
            succ->id());
        return false;
      }
    }
  }
  return true;
}

} // namespace jit::lir
