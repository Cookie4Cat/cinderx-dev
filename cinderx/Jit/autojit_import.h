// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace jit {

void autoJitImportEnter();
void autoJitImportLeave();
uint32_t autoJitImportDepth();
bool autoJitImportProviderEnabledFromEnv();

} // namespace jit
