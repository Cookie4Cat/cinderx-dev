// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "Python.h"

// Stable entry point for the later CPython 3.11 runtime integration.
#define CINDERX_PY311_VERSION_HEX 0x030B06F0

#ifdef __cplusplus
extern "C" {
#endif

PyObject* _Py_HOT_FUNCTION Ci_EvalFrameDefault_311(
    PyThreadState* tstate,
    struct _PyInterpreterFrame* frame,
    int throwflag);

#ifdef __cplusplus
}
#endif
