// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <memory>

namespace jit::hir {

class PrimitiveBoxRemat : public Pass {
 public:
  PrimitiveBoxRemat() : Pass("PrimitiveBoxRemat") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<PrimitiveBoxRemat> Factory() {
    return std::make_unique<PrimitiveBoxRemat>();
  }
};

} // namespace jit::hir
