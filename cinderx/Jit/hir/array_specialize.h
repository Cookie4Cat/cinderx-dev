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

// Get array.array for fast guards after runtime layout validation. CPython
// 3.11 only resolves an already-loaded module, or a live container through the
// interpreter's no-callback buffer validation. Other versions retain eager
// import on demand. Worker threads never initialize the cache.
PyTypeObject* getStdlibArrayType();

} // namespace jit::hir
