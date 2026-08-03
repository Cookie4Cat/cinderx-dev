// Copyright (c) Meta Platforms, Inc. and affiliates.

// The pristine 3.11 specializer is retained, but Release 34 does not export
// its dict-keys and function version allocators. Returning an existing version
// preserves already-specialized objects; returning zero otherwise invokes the
// upstream "out of versions" path and disables only that specialization.

#define Py_BUILD_CORE
#define NEED_OPCODE_TABLES

#include "cinderx/Interpreter/3.11/interpreter_dependencies.h"

static uint32_t Ci_GetExistingDictKeysVersion_311(PyDictKeysObject* keys) {
  return keys->dk_version;
}

static uint32_t Ci_GetExistingFunctionVersion_311(PyFunctionObject* function) {
  return function->func_version;
}

#define _PyDictKeys_GetVersionForCurrentState \
  Ci_GetExistingDictKeysVersion_311
#define _PyFunction_GetVersionForCurrentState \
  Ci_GetExistingFunctionVersion_311
#define _PyDictKeys_StringLookup Cix_PyDictKeys_StringLookup
#define _PyDict_GetItemHint Cix_PyDict_GetItemHint

#include "upstream/specialize.c"
