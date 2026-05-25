// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <memory>

namespace jit::hir {

class PrimitiveUnboxCSE : public Pass {
 public:
  PrimitiveUnboxCSE() : Pass("PrimitiveUnboxCSE") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<PrimitiveUnboxCSE> Factory() {
    return std::make_unique<PrimitiveUnboxCSE>();
  }
};

} // namespace jit::hir
