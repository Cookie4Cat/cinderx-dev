// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/float_accumulator_promotion.h"

#include "pycore_long.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/threaded_compile.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace jit::hir {

namespace {

std::optional<int64_t> getIntConst(Register* reg) {
  reg = chaseAssignOperand(reg);
  Type ty = reg->type();
  if (ty.hasIntSpec()) {
    return ty.intSpec();
  }
  if (!ty.hasObjectSpec() || !PyLong_CheckExact(ty.objectSpec())) {
    return std::nullopt;
  }

  int overflow = 0;
  long long value = PyLong_AsLongLongAndOverflow(ty.objectSpec(), &overflow);
  PyErr_Clear();
  if (overflow != 0) {
    return std::nullopt;
  }
  return static_cast<int64_t>(value);
}

bool isPromotableIntZero(Register* reg) {
  auto value = getIntConst(reg);
  return value.has_value() && *value == 0;
}

bool isPromotableFloatAccumulatorPhi(const Phi& phi) {
  bool saw_float = false;
  bool saw_zero = false;

  for (size_t i = 0, n = phi.NumOperands(); i < n; ++i) {
    Register* input = chaseAssignOperand(phi.GetOperand(i));
    if (input->isA(TFloatExact)) {
      saw_float = true;
      continue;
    }
    if (isPromotableIntZero(input)) {
      saw_zero = true;
      continue;
    }
    return false;
  }

  return saw_float && saw_zero;
}

Instr* firstNonPhi(BasicBlock* block) {
  for (auto& instr : *block) {
    if (!instr.IsPhi()) {
      return &instr;
    }
  }
  return nullptr;
}

} // namespace

void FloatAccumulatorPromotion::Run(Function& irfunc) {
  if (irfunc.cfg.entry_block == nullptr) {
    return;
  }

  // Boxing a compile-time float constant requires allocation; skip this
  // optimization under multi-threaded compile.
  RETURN_MULTITHREADED_COMPILE();

  Ref<> zero_float = Ref<>::steal(PyFloat_FromDouble(0.0));
  if (zero_float == nullptr) {
    PyErr_Clear();
    return;
  }
  Type zero_float_type =
      Type::fromObject(irfunc.env.addReference(std::move(zero_float)));

  bool changed = false;
  std::unordered_map<Phi*, Register*> promoted_phis;
  std::unordered_map<BasicBlock*, Register*> zero_consts;
  std::vector<std::unique_ptr<Instr>> removed_guards;

  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      Instr& instr = *it;
      ++it;

      if (!instr.IsGuardType()) {
        continue;
      }

      auto& guard = static_cast<GuardType&>(instr);
      if (guard.target() != TFloatExact) {
        continue;
      }

      Register* input = chaseAssignOperand(guard.GetOperand(0));
      if (!input->instr()->IsPhi()) {
        continue;
      }

      auto* phi = static_cast<Phi*>(input->instr());
      if (!isPromotableFloatAccumulatorPhi(*phi)) {
        continue;
      }

      Register* promoted = nullptr;
      auto promoted_it = promoted_phis.find(phi);
      if (promoted_it != promoted_phis.end()) {
        promoted = promoted_it->second;
      } else {
        std::unordered_map<BasicBlock*, Register*> args;
        for (size_t i = 0, n = phi->NumOperands(); i < n; ++i) {
          BasicBlock* pred = phi->basic_blocks()[i];
          Register* arg = chaseAssignOperand(phi->GetOperand(i));
          if (arg->isA(TFloatExact)) {
            args.emplace(pred, arg);
            continue;
          }

          JIT_CHECK(isPromotableIntZero(arg), "unexpected phi input in promotion");
          auto zero_it = zero_consts.find(pred);
          if (zero_it == zero_consts.end()) {
            Register* zero_reg = irfunc.env.AllocateRegister();
            auto* load_zero = LoadConst::create(zero_reg, zero_float_type);
            // Attribute synthetic 0.0 loads to the edge where they are
            // inserted.
            load_zero->copyBytecodeOffset(*pred->GetTerminator());
            load_zero->InsertBefore(*pred->GetTerminator());
            zero_it = zero_consts.emplace(pred, zero_reg).first;
          }
          args.emplace(pred, zero_it->second);
        }

        Register* out = irfunc.env.AllocateRegister();
        auto* promoted_phi = Phi::create(out, args);
        JIT_DCHECK(
            promoted_phi->basic_blocks() == phi->basic_blocks(),
            "promoted phi must preserve predecessor order");
        promoted_phi->copyBytecodeOffset(*phi);
        Instr* insert_point = firstNonPhi(phi->block());
        if (insert_point == nullptr) {
          insert_point = phi->block()->GetTerminator();
        }
        promoted_phi->InsertBefore(*insert_point);
        promoted = out;
        promoted_phis.emplace(phi, promoted);
      }

      auto* assign = Assign::create(guard.output(), promoted);
      assign->copyBytecodeOffset(guard);
      guard.ReplaceWith(*assign);
      removed_guards.emplace_back(&guard);
      changed = true;
    }
  }

  if (!changed) {
    return;
  }

  CopyPropagation{}.Run(irfunc);
  reflowTypes(irfunc);
}

} // namespace jit::hir
