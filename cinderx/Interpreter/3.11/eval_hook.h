// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  // The entry point is no longer ours: either restored or never taken.
  CI_EVAL_HOOK_REMOVED,
  // Another component replaced our evaluator while it was installed, so the
  // saved pointer was deliberately not written back.
  CI_EVAL_HOOK_OWNERSHIP_LOST,
  CI_EVAL_HOOK_ERROR,
} Ci_EvalHook311_RemoveResult;

// Install Ci_EvalFrame as the PEP 523 entry point, saving the evaluator it
// replaces.  Idempotent when already installed; fails without changing the
// entry point when a third party owns it.  Requires the GIL and the main
// interpreter.  Returns 0 on success, -1 with an exception set on failure.
int Ci_EvalHook311_Install(void);

// Restore the saved evaluator.  Requires the GIL.
Ci_EvalHook311_RemoveResult Ci_EvalHook311_Remove(void);

int Ci_EvalHook311_IsInstalled(void);

// Number of completed installations, for diagnostics.
uint64_t Ci_EvalHook311_Generation(void);

#ifdef __cplusplus
}
#endif
