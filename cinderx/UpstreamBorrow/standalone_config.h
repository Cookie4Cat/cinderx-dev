// Copyright (c) Meta Platforms, Inc. and affiliates.

// Configuration overlay for a synthesized standalone compilation database.
//
// Installed CPython headers can define WITH_DTRACE without installing the
// generated pydtrace_probes.h used to build the interpreter. Borrow extraction
// does not depend on those probes, so disable that build-only facility after
// loading the target interpreter's real pyconfig.h.

#include "pyconfig.h"
#undef WITH_DTRACE
