// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace jit::hir {

// Memory layout mirrors CPython arraymodule.c arrayobject and arraydescr.
// Used by the array.array('d') subscript fast paths emitted in the Simplify
// pass (see simplifyBinaryOp / simplifyStoreSubscr).
struct StdlibArrayDescr {
  char typecode;
  int itemsize;
};

struct StdlibArrayObject {
  PyObject_VAR_HEAD
  char* ob_item;
  Py_ssize_t allocated;
  const StdlibArrayDescr* ob_descr;
  PyObject* weakreflist;
  Py_ssize_t ob_exports;
};

// Get the array.array type object for fast path type guards. Pre-cached with
// runtime layout validation; returns nullptr when the array module cannot be
// imported, the layout cannot be validated, or compilation is running on a
// worker thread (the cache is only initialized on the main thread).
PyTypeObject* getStdlibArrayType();

} // namespace jit::hir
