// Copyright (c) Meta Platforms, Inc. and affiliates.

#define CINDERX_INTERPRETER

#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/module_c_state.h"

#include "internal/pycore_frame.h"

extern int Ci_SeedDictVersionShadow(void);

void Ci_InitOpcodes() {
  // See [P2] in cinderx_ceval.c: park the vendored loop's dict-version
  // shadow counter far above the runtime's own allocator.
  (void)Ci_SeedDictVersionShadow();
}

// The vendored upstream v3.11.6 eval loop (Interpreter/3.11/cinderx_ceval.c).
extern PyObject* Ci_EvalFrameDefault_311(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    int throwflag);

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState* tstate, _PyInterpreterFrame* frame, int throwflag) {
  // Route through the vendored 3.11.6 loop. Call counting and auto-JIT
  // scheduling land with the bytecode-frontend milestone.
  return Ci_EvalFrameDefault_311(tstate, frame, throwflag);
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
