// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/target_select.h"

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"

#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>

namespace jit::lir {
namespace {

#if defined(CINDER_X86_64)
void selectX64Opcodes(Function* func) {
  (void)func;
}
#elif defined(CINDER_AARCH64)
using UseCounts = std::unordered_map<const Instruction*, size_t>;

void countOperandUse(UseCounts& use_counts, const OperandBase* operand) {
  if (operand->isLinked()) {
    use_counts[static_cast<const LinkedOperand*>(operand)->getLinkedInstr()]++;
    return;
  }

  if (!operand->isInd()) {
    return;
  }

  MemoryIndirect* indirect = operand->getMemoryIndirect();
  OperandBase* base = indirect->getBaseRegOperand();
  if (base->isLinked()) {
    use_counts[static_cast<LinkedOperand*>(base)->getLinkedInstr()]++;
  }

  OperandBase* index = indirect->getIndexRegOperand();
  if (index != nullptr && index->isLinked()) {
    use_counts[static_cast<LinkedOperand*>(index)->getLinkedInstr()]++;
  }
}

/* Count the number of uses of each instruction in the function. This
 * information should really be stored in the IR so that we have either use
 * counts or use-def chains, but for now in order to get this functional we're
 * going to count the uses here.
 */
UseCounts countUses(Function* func) {
  UseCounts use_counts;
  for (BasicBlock* block : func->basicblocks()) {
    for (std::unique_ptr<Instruction>& instr : block->instructions()) {
      instr->foreachInputOperand([&use_counts](const OperandBase* operand) {
        countOperandUse(use_counts, operand);
      });
      // A store's output is a MemoryIndirect, but its base and index are
      // ordinary input uses for dataflow purposes.
      if (instr->output()->isInd()) {
        countOperandUse(use_counts, instr->output());
      }
    }
  }
  return use_counts;
}

bool hasSingleUse(const UseCounts& use_counts, const Instruction* instr) {
  auto iter = use_counts.find(instr);
  return iter != use_counts.end() && iter->second == 1;
}

bool isLinkedTo(const OperandBase* operand, const Instruction* instr) {
  return operand != nullptr && operand->isLinked() &&
      static_cast<const LinkedOperand*>(operand)->getLinkedInstr() == instr;
}

bool isIntegerGpValue(const OperandBase* operand, DataType data_type) {
  if (operand == nullptr || operand->dataType() != data_type ||
      (data_type != DataType::k32bit && data_type != DataType::k64bit)) {
    return false;
  }
  return operand->isVreg() ||
      (operand->isReg() && operand->getPhyRegister().is_gp_register());
}

bool isAddressValue(const OperandBase* operand, bool allow_sp) {
  if (operand == nullptr ||
      (operand->dataType() != DataType::k64bit &&
       operand->dataType() != DataType::kObject)) {
    return false;
  }
  if (operand->isVreg()) {
    return true;
  }
  if (!operand->isReg()) {
    return false;
  }
  auto location = operand->getPhyRegister();
  // Register encoding 31 is XZR as an arithmetic value but SP in the base
  // field of a load/store address.  Reject it here so address folding cannot
  // silently change a zero-valued add operand into the stack pointer.
  if (location.loc == PhyLocation::XZR) {
    return false;
  }
  return location.is_gp_register() ||
      (allow_sp && location.loc == PhyLocation::SP);
}

using AddressRegister = std::variant<Instruction*, PhyLocation>;

std::optional<AddressRegister> asAddressRegister(OperandBase* operand) {
  if (operand->isLinked()) {
    return static_cast<LinkedOperand*>(operand)->getLinkedInstr();
  }
  if (operand->isReg()) {
    return operand->getPhyRegister();
  }
  return std::nullopt;
}

OperandBase* ordinaryMoveMemoryOperand(Instruction* instr) {
  if (instr->opcode() != Instruction::kMove || instr->getNumInputs() != 1) {
    return nullptr;
  }

  bool memory_input = instr->getInput(0)->isInd();
  bool memory_output = instr->output()->isInd();
  if (memory_input == memory_output) {
    return nullptr;
  }
  return memory_input ? instr->getInput(0) : instr->output();
}

/* Fold an adjacent, single-use integer multiply into an add or subtract:
 *
 *     product = Mul lhs, rhs
 *     result = Add product, accumulator
 *
 * becomes MulAdd(lhs, rhs, accumulator).  The subtraction form is accepted
 * only as accumulator - product, matching AArch64 MSUB semantics.
 */
void selectA64MulArithmetic(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* multiply = instr_iter->get();
  if (!multiply->isMul() || multiply->getNumInputs() != 2 ||
      !multiply->output()->isVreg() || !hasSingleUse(use_counts, multiply)) {
    return;
  }

  auto consumer_iter = std::next(instr_iter);
  if (consumer_iter == block->instructions().end()) {
    return;
  }
  Instruction* consumer = consumer_iter->get();
  if ((!consumer->isAdd() && !consumer->isSub()) ||
      consumer->getNumInputs() != 2) {
    return;
  }

  size_t product_index = 0;
  size_t accumulator_index = 0;
  Instruction::Opcode replacement = Instruction::kMulAdd;
  if (consumer->isAdd()) {
    if (isLinkedTo(consumer->getInput(0), multiply)) {
      product_index = 0;
      accumulator_index = 1;
    } else if (isLinkedTo(consumer->getInput(1), multiply)) {
      product_index = 1;
      accumulator_index = 0;
    } else {
      return;
    }
  } else {
    // msub Rd, Rn, Rm, Ra computes Ra - Rn * Rm.  Do not rewrite the
    // non-equivalent product - accumulator form.
    if (!isLinkedTo(consumer->getInput(1), multiply)) {
      return;
    }
    product_index = 1;
    accumulator_index = 0;
    replacement = Instruction::kMulSub;
  }

  DataType data_type = multiply->output()->dataType();
  if (!isIntegerGpValue(multiply->output(), data_type) ||
      !isIntegerGpValue(multiply->getInput(0), data_type) ||
      !isIntegerGpValue(multiply->getInput(1), data_type) ||
      !isIntegerGpValue(consumer->output(), data_type) ||
      !isIntegerGpValue(consumer->getInput(product_index), data_type) ||
      !isIntegerGpValue(consumer->getInput(accumulator_index), data_type)) {
    return;
  }

  auto accumulator = consumer->removeInput(accumulator_index);
  // Removing either accumulator position leaves the product as input zero.
  consumer->removeInput(0);
  auto lhs = multiply->removeInput(0);
  auto rhs = multiply->removeInput(0);

  consumer->setOpcode(replacement);
  consumer->appendInput(std::move(lhs));
  consumer->appendInput(std::move(rhs));
  consumer->appendInput(std::move(accumulator));
  block->removeInstr(instr_iter);
}

/* Fold a single-use shifted index into the scaled register addressing mode
 * already understood by MemoryIndirect and AArch64 code generation.
 */
void selectA64ShiftedAddress(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* shift = instr_iter->get();
  if (!shift->isLShift() || shift->getNumInputs() != 2 ||
      !shift->output()->isVreg() || !hasSingleUse(use_counts, shift) ||
      !isAddressValue(shift->output(), false) ||
      !isAddressValue(shift->getInput(0), false)) {
    return;
  }

  OperandBase* shift_amount = shift->getInput(1);
  if (!shift_amount->isImm() || shift_amount->isFp()) {
    return;
  }

  auto memory_iter = std::next(instr_iter);
  if (memory_iter == block->instructions().end()) {
    return;
  }
  OperandBase* memory_operand = ordinaryMoveMemoryOperand(memory_iter->get());
  if (memory_operand == nullptr) {
    return;
  }

  DataType access_type = memory_operand->dataType();
  if (access_type != DataType::k32bit && access_type != DataType::k64bit &&
      access_type != DataType::kObject) {
    return;
  }

  MemoryIndirect* address = memory_operand->getMemoryIndirect();
  OperandBase* base = address->getBaseRegOperand();
  OperandBase* index = address->getIndexRegOperand();
  if (!isAddressValue(base, true) || !isLinkedTo(index, shift) ||
      isLinkedTo(base, shift) || address->getMultipiler() != 0 ||
      address->getOffset() != 0) {
    return;
  }

  uint64_t shift_value = shift_amount->getConstant();
  if (shift_value == 0 || shift_value > 3 ||
      shift_value != byteShift(access_type)) {
    return;
  }

  auto base_register = asAddressRegister(base);
  auto index_register = asAddressRegister(shift->getInput(0));
  if (!base_register.has_value() || !index_register.has_value()) {
    return;
  }

  address->setMemoryIndirect(
      *base_register, *index_register, static_cast<uint8_t>(shift_value), 0);
  block->removeInstr(instr_iter);
}

/* Fold a single-use address addition into a base-plus-index memory operand:
 *
 *     address = Add base, index
 *     value = Move [address]
 *
 * Both input orders are accepted, but SP may only be selected as the base.
 */
void selectA64AddedAddress(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* add = instr_iter->get();
  if (!add->isAdd() || add->getNumInputs() != 2 || !add->output()->isVreg() ||
      !hasSingleUse(use_counts, add) || !isAddressValue(add->output(), false)) {
    return;
  }

  auto memory_iter = std::next(instr_iter);
  if (memory_iter == block->instructions().end()) {
    return;
  }
  OperandBase* memory_operand = ordinaryMoveMemoryOperand(memory_iter->get());
  if (memory_operand == nullptr) {
    return;
  }

  DataType access_type = memory_operand->dataType();
  if (access_type != DataType::k32bit && access_type != DataType::k64bit &&
      access_type != DataType::kObject) {
    return;
  }

  MemoryIndirect* address = memory_operand->getMemoryIndirect();
  if (!isLinkedTo(address->getBaseRegOperand(), add) ||
      address->getIndexRegOperand() != nullptr || address->getOffset() != 0) {
    return;
  }

  OperandBase* lhs = add->getInput(0);
  OperandBase* rhs = add->getInput(1);
  OperandBase* base = nullptr;
  OperandBase* index = nullptr;
  if (isAddressValue(lhs, true) && isAddressValue(rhs, false)) {
    base = lhs;
    index = rhs;
  } else if (isAddressValue(rhs, true) && isAddressValue(lhs, false)) {
    base = rhs;
    index = lhs;
  } else {
    return;
  }

  auto base_register = asAddressRegister(base);
  auto index_register = asAddressRegister(index);
  if (!base_register.has_value() || !index_register.has_value()) {
    return;
  }

  address->setMemoryIndirect(*base_register, *index_register, 0, 0);
  block->removeInstr(instr_iter);
}

/* Check that intervening instructions between two iterator points do not modify
 * flags in any way. This allows the two endpoints to reliably set/get flags. */
bool flagsPreservedBetween(instr_iter_t begin, instr_iter_t end) {
  for (instr_iter_t iter = begin; iter != end; iter++) {
    if (InstrProperty::getProperties(iter->get()->opcode()).flag_effects !=
        FlagEffects::kNone) {
      return false;
    }
  }
  return true;
}

/* AArch64 GPR operations produce at least 32-bit results. Keep semantic
 * sub-32-bit types in generic LIR, then legalize them before register
 * allocation so codegen does not need to mask partial-register results.
 */
void legalizeA64Min32BitOutput(instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  if (instr->output()->sizeInBits() < 32) {
    instr->output()->setDataType(DataType::k32bit);
  }
}

/* AArch64 signed operations on sub-32-bit values need sign-extension. LIR
 * DataType doesn't track signedness, so values in registers are zero-extended
 * by default. Signed comparisons and signed division need explicit 32-bit
 * signed inputs for correctness.
 */
void legalizeA64SignedSubWordInputs(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->opcode() == Instruction::kGreaterThanSigned ||
          instr->opcode() == Instruction::kGreaterThanEqualSigned ||
          instr->opcode() == Instruction::kLessThanSigned ||
          instr->opcode() == Instruction::kLessThanEqualSigned ||
          instr->opcode() == Instruction::kDiv,
      "Expected signed comparison or Div, got {}",
      instr->opname());

  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    OperandBase* input = instr->getInput(i);
    if (!input->isReg() && !input->isLinked()) {
      continue;
    }

    DataType dt = input->dataType();
    if (dt != OperandBase::k8bit && dt != OperandBase::k16bit) {
      continue;
    }

    Instruction* sext = block->allocateInstrBefore(
        instr_iter, Instruction::kSext, OutVReg{DataType::k32bit});
    sext->appendInput(instr->releaseInput(i));
    instr->setInput(i, std::make_unique<LinkedOperand>(sext));
  }
}

/* AArch64 cannot directly test-and-branch on FP registers. Move double guard
 * inputs through a GP-sized vreg before guard selection and register
 * allocation.
 */
void legalizeA64GuardFPInput(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isGuard(), "Expected Guard, got {}", instr->opname());

  constexpr size_t kGuardVarIndex = 2;
  OperandBase* guard_var = instr->getInput(kGuardVarIndex);
  if (guard_var->dataType() != DataType::kDouble) {
    return;
  }

  Instruction* move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg{DataType::k64bit});
  move->appendInput(instr->releaseInput(kGuardVarIndex));
  instr->setInput(kGuardVarIndex, std::make_unique<LinkedOperand>(move));
}

Instruction* moveA64StackInputToVreg(
    BasicBlock* block,
    instr_iter_t instr_iter,
    size_t idx) {
  Instruction* instr = instr_iter->get();
  OperandBase* input = instr->getInput(idx);
  JIT_DCHECK(input->isStack(), "Expected stack input");

  PhyLocation loc = input->getStackSlot();
  DataType dt = input->dataType();
  Instruction* move = block->allocateInstrBefore(
      instr_iter, Instruction::kMove, OutVReg{dt}, Stk{loc, dt});
  instr->setInput(idx, std::make_unique<LinkedOperand>(move));
  return move;
}

/* AArch64 unary arithmetic instructions only operate on registers. */
void legalizeA64UnaryStackInput(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->isNegate() || instr->isInvert(),
      "Expected Negate or Invert, got {}",
      instr->opname());

  if (!instr->getInput(0)->isStack()) {
    return;
  }

  moveA64StackInputToVreg(block, instr_iter, 0);
}

/* AArch64 Select lowers to register-only csel. */
void legalizeA64SelectStackInputs(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isSelect(), "Expected Select, got {}", instr->opname());

  for (size_t i = 0; i < instr->getNumInputs(); i++) {
    if (instr->getInput(i)->isStack()) {
      moveA64StackInputToVreg(block, instr_iter, i);
    }
  }
}

/* AArch64 Inc/Dec only operate on registers. Rewrite stack updates through a
 * virtual register so register allocation handles the temporary.
 */
void legalizeA64StackInputForIncDec(
    BasicBlock* block,
    instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(
      instr->isInc() || instr->isDec(),
      "Expected Inc or Dec, got {}",
      instr->opname());

  OperandBase* input = instr->getInput(0);
  if (!input->isStack()) {
    return;
  }

  PhyLocation loc = input->getStackSlot();
  DataType dt = input->dataType();
  Instruction* move = moveA64StackInputToVreg(block, instr_iter, 0);

  block->allocateInstrBefore(
      std::next(instr_iter), Instruction::kMove, OutStk{loc, dt}, VReg{move});
}

bool sameGpValue(
    const OperandBase* left,
    const OperandBase* right,
    DataType data_type) {
  if (!isIntegerGpValue(left, data_type) ||
      !isIntegerGpValue(right, data_type)) {
    return false;
  }
  if (left->isLinked() || right->isLinked()) {
    return left->isLinked() && right->isLinked() &&
        left->getDefine() == right->getDefine();
  }
  return left->getPhyRegister() == right->getPhyRegister();
}

/* Select SUBS when an immediately preceding subtraction computes the same
 * operands as a compare.  The caller is responsible for removing the compare
 * after detaching its sole branch/guard use.
 */
bool selectA64SubSetFlags(
    BasicBlock* block,
    instr_iter_t compare_iter,
    Instruction* compare) {
  if (compare_iter == block->instructions().begin() ||
      compare->getNumInputs() != 2) {
    return false;
  }

  Instruction* sub = std::prev(compare_iter)->get();
  // This transform runs before register allocation, where a VReg definition
  // names the value consumed by the following compare.  A physical-register
  // output can overwrite one of its inputs before the compare reads it (for
  // example, x0 = Sub x0, x1; Cmp x0, x1), so comparing register numbers is
  // not sufficient to prove value identity for that shape.
  if (!sub->isSub() || sub->getNumInputs() != 2 || !sub->output()->isVreg()) {
    return false;
  }

  DataType data_type = sub->output()->dataType();
  if (!isIntegerGpValue(sub->output(), data_type) ||
      !sameGpValue(sub->getInput(0), compare->getInput(0), data_type) ||
      !sameGpValue(sub->getInput(1), compare->getInput(1), data_type)) {
    return false;
  }

  sub->setOpcode(Instruction::kA64SubSetFlags);
  return true;
}

/* Convert from:
 *
 *     cmp x0, x1
 *     cset w2, eq
 *     b.eq label
 *
 * to:
 *
 *     cmp x0, x1
 *     b.eq label
 */
void selectA64CondBranch(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* branch = instr_iter->get();
  JIT_DCHECK(
      branch->isCondBranch(), "Expected CondBranch, got {}", branch->opname());

  /* Check that the input to this conditional branch is not a def. */
  OperandBase* input = branch->getInput(0);
  if (!input->isLinked()) {
    return;
  }

  /* Check that the input to this conditional branch is a compare in the same
   * block and that it is not used by any other instruction. */
  Instruction* compare = static_cast<LinkedOperand*>(input)->getLinkedInstr();
  if (!compare->isCompare() || compare->basicblock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Check that the instructions between the compare and the conditional branch
   * do not modify flags. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!flagsPreservedBetween(std::next(compare_iter), instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Instruction::Opcode branch_opcode =
      Instruction::compareToBranchCC(compare->opcode());

  bool fused_sub = selectA64SubSetFlags(block, compare_iter, compare);
  branch->setOpcode(branch_opcode);
  branch->setNumInputs(0);
  if (fused_sub) {
    block->removeInstr(compare_iter);
  } else {
    compare->setOpcode(Instruction::kCmp);
    compare->output()->setNone();
  }
}

/* Convert from:
 *
 *     addr = Lea [base + index * (1 << mult) + offset]  where mult >= 4
 *
 * to:
 *
 *     scale = Move(Imm(1 << mult))
 *     addr' = MulAdd(index, scale, base)
 *     [if offset != 0: addr' = Add(addr', Imm(offset))]
 *     addr = Move(addr')
 */
void selectA64LeaLargeMultiplier(BasicBlock* block, instr_iter_t instr_iter) {
  Instruction* instr = instr_iter->get();
  JIT_DCHECK(instr->isLea(), "Expected Lea, got {}", instr->opname());

  OperandBase* input = instr->getInput(0);
  if (!input->isInd()) {
    return;
  }

  MemoryIndirect* ind = input->getMemoryIndirect();
  OperandBase* index_op = ind->getIndexRegOperand();
  if (index_op == nullptr) {
    return;
  }

  uint8_t mult = ind->getMultipiler();
  if (mult < 4) {
    return;
  }

  int32_t offset = ind->getOffset();

  std::unique_ptr<OperandBase> ind_input = instr->removeInput(0);
  ind = ind_input->getMemoryIndirect();
  std::unique_ptr<OperandBase> index = ind->releaseIndexRegOperand();
  std::unique_ptr<OperandBase> base = ind->releaseBaseRegOperand();
  JIT_CHECK(base != nullptr, "Expected Lea with index to also have a base");

  Instruction* scale_move = block->allocateInstrBefore(
      instr_iter,
      Instruction::kMove,
      OutVReg{DataType::k64bit},
      Imm{uint64_t{1} << mult, DataType::k64bit});

  Instruction* muladd = block->allocateInstrBefore(
      instr_iter, Instruction::kMulAdd, OutVReg{DataType::k64bit});
  muladd->appendInput(std::move(index));
  muladd->appendInput(std::make_unique<LinkedOperand>(scale_move));
  muladd->appendInput(std::move(base));

  Instruction* final_result = muladd;
  if (offset != 0) {
    uint64_t offset_value = static_cast<uint64_t>(static_cast<int64_t>(offset));

    Instruction* offset_move = nullptr;
    if (!asmjit::arm::Utils::isAddSubImm(offset_value)) {
      offset_move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{DataType::k64bit},
          Imm{offset_value, DataType::k64bit});
    }

    Instruction* add = block->allocateInstrBefore(
        instr_iter, Instruction::kAdd, OutVReg{DataType::k64bit}, VReg{muladd});
    if (offset_move == nullptr) {
      add->addOperands(Imm{offset_value, DataType::k64bit});
    } else {
      add->addOperands(VReg{offset_move});
    }

    final_result = add;
  }

  instr->setOpcode(Instruction::kMove);
  instr->appendInput(std::make_unique<LinkedOperand>(final_result));
}

/* Convert from:
 *
 *     cmp x0, x1
 *     cset w2, lt
 *     cbz w2, deopt
 *
 * to:
 *
 *     cmp x0, x1
 *     b.ge deopt
 */
void selectA64Guard(
    BasicBlock* block,
    instr_iter_t instr_iter,
    const UseCounts& use_counts) {
  Instruction* guard = instr_iter->get();
  JIT_DCHECK(guard->isGuard(), "Expected Guard, got {}", guard->opname());

  /* Check that the guard kind is a zero or not zero check. */
  InstrGuardKind kind =
      static_cast<InstrGuardKind>(guard->getInput(0)->getConstant());
  if (kind != InstrGuardKind::kNotZero && kind != InstrGuardKind::kZero) {
    return;
  }

  /* Check that the input to this guard is not a def. */
  OperandBase* input = guard->getInput(2);
  if (!input->isLinked()) {
    return;
  }

  /* Check that the input to this guard is a compare in the same block and that
   * it is not used by any other instruction. */
  Instruction* compare = static_cast<LinkedOperand*>(input)->getLinkedInstr();
  if (!compare->isCompare() || compare->basicblock() != block ||
      use_counts.at(compare) != 1) {
    return;
  }

  /* Check that the instructions between the compare and the guard do not
   * modify flags. */
  instr_iter_t compare_iter = block->iterator_to(compare);
  if (!flagsPreservedBetween(std::next(compare_iter), instr_iter)) {
    return;
  }

  /* Convert to a conditional compare and branch instruction. */
  Instruction::Opcode branch_opcode =
      Instruction::compareToBranchCC(compare->opcode());
  if (kind == InstrGuardKind::kNotZero) {
    branch_opcode = Instruction::negateBranchCC(branch_opcode);
  }

  bool fused_sub = selectA64SubSetFlags(block, compare_iter, compare);

  guard->setOpcode(Instruction::kA64GuardCC);
  static_cast<Operand*>(guard->getInput(0))
      ->setConstant(static_cast<uint64_t>(branch_opcode));
  // A64GuardCC branches using the condition encoded above. The original Guard
  // variable and target operands are no longer needed.
  guard->removeInput(3);
  guard->removeInput(2);
  if (fused_sub) {
    block->removeInstr(compare_iter);
  } else {
    compare->setOpcode(Instruction::kCmp);
    compare->output()->setNone();
  }
}

void selectA64Opcodes(Function* func) {
  UseCounts use_counts = countUses(func);
  for (BasicBlock* block : func->basicblocks()) {
    BasicBlock::InstrList& instrs = block->instructions();
    for (instr_iter_t iter = instrs.begin(); iter != instrs.end();) {
      instr_iter_t cur_iter = iter++;
      switch (cur_iter->get()->opcode()) {
        case Instruction::kEqual:
        case Instruction::kNotEqual:
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Instruction::kGreaterThanSigned:
        case Instruction::kGreaterThanEqualSigned:
        case Instruction::kLessThanSigned:
        case Instruction::kLessThanEqualSigned:
          legalizeA64SignedSubWordInputs(block, cur_iter);
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Instruction::kGreaterThanUnsigned:
        case Instruction::kGreaterThanEqualUnsigned:
        case Instruction::kLessThanUnsigned:
        case Instruction::kLessThanEqualUnsigned:
        case Instruction::kAnd:
        case Instruction::kXor:
        case Instruction::kOr:
          legalizeA64Min32BitOutput(cur_iter);
          break;
        case Instruction::kDiv:
          legalizeA64SignedSubWordInputs(block, cur_iter);
          break;
        case Instruction::kLea:
          selectA64LeaLargeMultiplier(block, cur_iter);
          break;
        case Instruction::kMul:
          selectA64MulArithmetic(block, cur_iter, use_counts);
          break;
        case Instruction::kLShift:
          selectA64ShiftedAddress(block, cur_iter, use_counts);
          break;
        case Instruction::kAdd:
          selectA64AddedAddress(block, cur_iter, use_counts);
          break;
        case Instruction::kCondBranch:
          selectA64CondBranch(block, cur_iter, use_counts);
          break;
        case Instruction::kGuard:
          legalizeA64GuardFPInput(block, cur_iter);
          selectA64Guard(block, cur_iter, use_counts);
          break;
        case Instruction::kNegate:
        case Instruction::kInvert:
          legalizeA64UnaryStackInput(block, cur_iter);
          break;
        case Instruction::kSelect:
          legalizeA64SelectStackInputs(block, cur_iter);
          break;
        case Instruction::kInc:
        case Instruction::kDec:
          legalizeA64StackInputForIncDec(block, cur_iter);
          break;
        default:
          break;
      }
    }
  }
}
#else
void selectUnknownTargetOpcodes(Function* func) {
  (void)func;
}
#endif

} // namespace

void selectTargetOpcodes(Function* func) {
#if defined(CINDER_X86_64)
  selectX64Opcodes(func);
#elif defined(CINDER_AARCH64)
  selectA64Opcodes(func);
#else
  selectUnknownTargetOpcodes(func);
#endif
}

} // namespace jit::lir
