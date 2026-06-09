// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>

namespace jit {

void autoJitImportEnter();
void autoJitImportLeave();
uint32_t autoJitImportDepth();
void autoJitSetupEnter();
void autoJitSetupLeave();
uint32_t autoJitSetupDepth();
uint32_t autoJitStartupDepth();
bool autoJitImportProviderEnabledFromEnv();

} // namespace jit
