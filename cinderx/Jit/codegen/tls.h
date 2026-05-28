// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace jit::codegen {
std::optional<int32_t> parseThreadStatePrologue(
    const uint32_t* insns,
    size_t count);
void initThreadStateOffset();
}
