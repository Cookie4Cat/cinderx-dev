// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// The reason every CPython 3.11 compile request terminates with: the
// capability gate fires before any bytecode is read.
#define CI_OBSERVE_311_REFUSAL "CINDERX311_JIT_EXEC_DISABLED"

// Non-zero once observe mode is configured on.  Read directly on the frame
// entry so the JIT-off hot path pays one predictable flag test and nothing
// else.
extern int Ci_Observe311_Enabled;

// Parse CINDERX_JIT_MODE, the PYTHONJITAUTO threshold and
// CINDERX_JIT_OBSERVE_FILE.  A successful parse is recorded once and kept;
// a failed one records nothing, sets an exception and returns -1, and a
// later call parses again.
int Ci_Observe311_Configure(void);

// Frame-entry hot counting: one scheduling request per code object crossing
// the threshold, walked into Ci_JitShell311_RequestCompile.  Never changes
// what the frame computes.
void Ci_Observe311_OnFrame(PyCodeObject* code);

// Snapshot dict for tests and diagnostics: enabled, threshold, codes_seen,
// events_dropped, and the bounded event list (qualname, count, result).
PyObject* Ci_Observe311_Stats(void);

// The JIT shell's unified compile entry point.  In this build it refuses
// before reading any bytecode and returns the refusal reason; an
// execution-capable shell would replace the body, not the callers.
const char* Ci_JitShell311_RequestCompile(PyCodeObject* code);

#ifdef __cplusplus
}
#endif
