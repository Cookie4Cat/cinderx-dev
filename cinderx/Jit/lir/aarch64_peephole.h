// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/rewrite.h"

namespace jit::lir {

#if defined(CINDER_AARCH64)
// Register the single AArch64 peephole dispatcher with the existing
// post-register-allocation rewrite pass.  Individual rules stay behind this
// dispatcher so adding rules does not add one global callback per rule.
void registerAArch64PeepholeRewrites(Rewrite& rewrite);
#endif

} // namespace jit::lir
