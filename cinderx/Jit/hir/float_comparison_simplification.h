// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <memory>

namespace jit::hir {

class FloatComparisonSimplification : public Pass {
 public:
  FloatComparisonSimplification()
      : Pass("FloatComparisonSimplification") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<FloatComparisonSimplification> Factory() {
    return std::make_unique<FloatComparisonSimplification>();
  }
};

} // namespace jit::hir
