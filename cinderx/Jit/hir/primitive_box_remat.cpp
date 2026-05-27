// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/primitive_box_remat.h"

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

namespace {

struct RematCandidate {
  PrimitiveBox* box;
  Register* boxed;
  Register* unboxed;
  std::vector<Instr*> removable_uses;
};

bool collectRemovableUses(
    const RegUses& direct_uses,
    Register* boxed,
    std::vector<Instr*>& removable_uses) {
  auto use_it = direct_uses.find(boxed);
  if (use_it == direct_uses.end()) {
    return true;
  }

  for (Instr* use : use_it->second) {
    if (!use->IsUseType()) {
      return false;
    }
    removable_uses.push_back(use);
  }
  return true;
}

std::unordered_set<Register*> replaceInAllFrameStates(
    Function& func,
    const std::unordered_map<Register*, Register*>& replacements) {
  std::unordered_set<Register*> replaced;
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      FrameState* fs = get_frame_state(instr);
      if (fs == nullptr) {
        continue;
      }
      fs->visitUses([&](Register*& reg) {
        auto it = replacements.find(reg);
        if (it != replacements.end()) {
          replaced.insert(reg);
          reg = it->second;
        }
        return true;
      });
    }
  }
  return replaced;
}

} // namespace

void PrimitiveBoxRemat::Run(Function& irfunc) {
  bool changed = false;
  auto direct_uses = collectDirectRegUses(irfunc);
  std::vector<RematCandidate> candidates;
  std::unordered_map<Register*, Register*> replacements;

  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.IsPrimitiveBox()) {
        continue;
      }

      auto& box = static_cast<PrimitiveBox&>(instr);
      if (box.type() != TCDouble) {
        continue;
      }

      Register* boxed = box.output();
      Register* unboxed = box.value();
      if (unboxed == nullptr || !(unboxed->type() <= TCDouble)) {
        continue;
      }

      std::vector<Instr*> removable_uses;
      if (!collectRemovableUses(direct_uses, boxed, removable_uses)) {
        continue;
      }

      candidates.push_back(
          RematCandidate{&box, boxed, unboxed, std::move(removable_uses)});
      replacements.emplace(boxed, unboxed);
    }
  }

  if (candidates.empty()) {
    return;
  }

  auto replaced = replaceInAllFrameStates(irfunc, replacements);
  std::vector<std::unique_ptr<Instr>> removed_instrs;

  for (auto& candidate : candidates) {
    if (!replaced.contains(candidate.boxed)) {
      continue;
    }

    for (Instr* use : candidate.removable_uses) {
      use->unlink();
      removed_instrs.emplace_back(use);
    }

    candidate.box->unlink();
    removed_instrs.emplace_back(candidate.box);
    changed = true;
  }

  if (changed) {
    CopyPropagation{}.Run(irfunc);
    reflowTypes(irfunc);
  }
}

} // namespace jit::hir
