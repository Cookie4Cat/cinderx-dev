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

  // 3.11 has no immortal objects (PEP 683 is 3.12+), but JIT codegen
  // borrows the singletons without increfs on paths where 3.12+ relies on
  // immortality (e.g. PrimitiveBoxBool selecting Py_True/Py_False as a
  // "new" reference). Pseudo-immortalize them via the python.h shim so
  // every such path is safe wholesale; small ints are not included because
  // their JIT paths carry explicit increfs and blanket-immortalizing them
  // would blind refleak tooling.
  _Py_SetImmortal(Py_True);
  _Py_SetImmortal(Py_False);
  _Py_SetImmortal(Py_None);
  _Py_SetImmortal(Py_NotImplemented);
  _Py_SetImmortal(Py_Ellipsis);
}

// The vendored upstream v3.11.6 eval loop (Interpreter/3.11/cinderx_ceval.c).
extern PyObject* Ci_EvalFrameDefault_311(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    int throwflag);

// pyjit.cpp：函数槽位仍为解释器默认入口时安装 auto-JIT 计数入口。
extern int Ci_MaybeInstallAutoJitEntry311(PyFunctionObject* func);

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState* tstate, _PyInterpreterFrame* frame, int throwflag) {
  // 3.11 无 function watcher：auto-JIT 引导在此惰性接线——函数首次以
  // 解释方式进入求值器时安装计数入口，后续调用经 vectorcall 计数并在
  // 达到阈值后编译（vendored CALL 特化在自定义 eval_frame 下 DEOPT 走
  // 通用调用路径，故所有 Python 调用都会经过 vectorcall）。正式开发可
  // 为该检查加缓存快路径。
  if (frame->f_func != NULL) {
    (void)Ci_MaybeInstallAutoJitEntry311(frame->f_func);
  }
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
