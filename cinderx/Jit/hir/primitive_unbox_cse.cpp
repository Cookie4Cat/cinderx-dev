// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/primitive_unbox_cse.h"

#include "cinderx/Jit/hir/copy_propagation.h"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jit::hir {

namespace {

struct PrimitiveUnboxKey {
  Register* value;
  Type type;

  bool operator==(const PrimitiveUnboxKey& other) const {
    return value == other.value && type == other.type;
  }
};

struct PrimitiveUnboxKeyHash {
  std::size_t operator()(const PrimitiveUnboxKey& key) const {
    std::size_t h1 = std::hash<Register*>{}(key.value);
    std::size_t h2 = std::hash<Type>{}(key.type);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
  }
};

} // namespace

void PrimitiveUnboxCSE::Run(Function& irfunc) {
  bool changed = false;
  std::vector<std::unique_ptr<Instr>> removed_unboxes;

  for (auto& block : irfunc.cfg.blocks) {
    std::unordered_map<PrimitiveUnboxKey, Register*, PrimitiveUnboxKeyHash>
        available_unboxes;

    for (auto it = block.begin(); it != block.end();) {
      Instr& instr = *it;
      ++it;
      if (!instr.IsPrimitiveUnbox()) {
        continue;
      }

      auto& unbox = static_cast<PrimitiveUnbox&>(instr);
      Register* value = chaseAssignOperand(unbox.value());
      PrimitiveUnboxKey key{value, unbox.type()};
      auto existing = available_unboxes.find(key);
      if (existing == available_unboxes.end()) {
        available_unboxes.emplace(key, unbox.output());
        continue;
      }

      auto assign = Assign::create(unbox.output(), existing->second);
      assign->copyBytecodeOffset(unbox);
      unbox.ReplaceWith(*assign);
      removed_unboxes.emplace_back(&unbox);
      changed = true;
    }
  }

  if (changed) {
    CopyPropagation{}.Run(irfunc);
    reflowTypes(irfunc);
  }
}

} // namespace jit::hir
