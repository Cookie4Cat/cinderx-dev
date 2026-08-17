// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// The reason every CPython 3.11 observe-mode compile request terminates with:
// the capability gate fires before any bytecode is read.  Shadow mode uses
// the same scheduling path but continues through the discard-only backend.
#define CI_OBSERVE_311_REFUSAL "CINDERX311_JIT_EXEC_DISABLED"

// Non-zero once observe mode is configured on.  Read directly on the frame
// entry so the JIT-off hot path pays one predictable flag test and nothing
// else.
extern int Ci_Observe311_Enabled;

// Parse CINDERX_JIT_MODE (off, observe or shadow), the PYTHONJITAUTO
// threshold and CINDERX_JIT_OBSERVE_FILE.  A successful parse is recorded
// once and kept; a failed one records nothing, sets an exception and returns
// -1, and a later call parses again.
int Ci_Observe311_Configure(void);

// Frame-entry hot counting: one scheduling request per code object crossing
// the threshold, walked into Ci_JitShell311_RequestCompile with the real
// function object.  Never changes what the frame computes.
void Ci_Observe311_OnFrame(PyFunctionObject* func);

// Snapshot dict for tests and diagnostics: enabled, threshold, codes_seen,
// events_dropped, and the bounded event list (qualname, filename, count,
// result).
PyObject* Ci_Observe311_Stats(void);

// Release all observer-owned weakrefs, event references, tables and files.
// This also resets configuration so a later interpreter can configure anew.
void Ci_Observe311_Finalize(void);

// The JIT shell's unified compile entry point.  Observe mode returns a stable
// capability refusal; shadow mode synchronously compiles and discards the
// artifact without installing an entry point.
const char* Ci_JitShell311_RequestCompile(PyFunctionObject* func);

#ifdef __cplusplus
}
#endif
