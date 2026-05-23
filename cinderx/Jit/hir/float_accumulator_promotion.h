// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace jit::hir {

class FloatAccumulatorPromotion : public Pass {
 public:
  FloatAccumulatorPromotion() : Pass("FloatAccumulatorPromotion") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<FloatAccumulatorPromotion> Factory() {
    return std::make_unique<FloatAccumulatorPromotion>();
  }
};

} // namespace jit::hir
