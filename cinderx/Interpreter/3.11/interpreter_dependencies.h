// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "Python.h"
#include "internal/pycore_dict.h"
#include "internal/pycore_frame.h"

// Integration-time private dependencies. Their implementations intentionally
// do not belong to this standalone interpreter target.
PyObject* Cix_PyAsyncGenValueWrapperNew(PyObject* value);
_PyInterpreterFrame* Cix_PyThreadState_PushFrame(
    PyThreadState* tstate,
    size_t size);
uint64_t Cix_PyDict_NextVersion(void);
Py_ssize_t Cix_PyDictKeys_StringLookup(
    PyDictKeysObject* keys,
    PyObject* key);
Py_ssize_t Cix_PyDict_GetItemHint(
    PyDictObject* dict,
    PyObject* key,
    Py_ssize_t hint,
    PyObject** value);
