// Copyright (c) Meta Platforms, Inc. and affiliates.

// CinderX entry points for the CPython 3.11 interpreter source set.

#define CINDERX_INTERPRETER

#include "cinderx/Interpreter/interpreter.h"

#include "internal/pycore_frame.h"

#include "cinderx/Interpreter/3.11/interpreter_contract.h"

// 3.11 takes its opcode tables straight from the target interpreter, so there
// is no CinderX table to initialize.
void Ci_InitOpcodes() {}

// Static Python is out of scope on 3.11; these entry points exist because the
// shared runtime links against them, and they behave as ordinary calls.
PyObject* Ci_StaticFunction_Vectorcall(
    PyObject* func,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  return Ci_PyFunction_Vectorcall(func, stack, nargsf, kwnames);
}

PyObject* _Py_HOT_FUNCTION Ci_PyFunction_CallStatic(
    PyFunctionObject* func,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  return Ci_PyFunction_Vectorcall((PyObject*)func, args, nargsf, kwnames);
}
