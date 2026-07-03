// Copyright (c) Meta Platforms, Inc. and affiliates.

#define CINDERX_INTERPRETER

#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/module_c_state.h"

#include "internal/pycore_frame.h"

void Ci_InitOpcodes() {}

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState* tstate, _PyInterpreterFrame* frame, int throwflag) {
  // Plain PEP 523 pass-through on 3.11. Call counting and auto-JIT
  // scheduling land with the bytecode-frontend milestone.
  return _PyEval_EvalFrameDefault(tstate, frame, throwflag);
}

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
