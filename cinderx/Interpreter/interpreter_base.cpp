// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_function.h"
#endif

#if PY_VERSION_HEX < 0x030D0000 && defined(ENABLE_EVAL_HOOK)
#include "cinder/hooks.h"
#endif

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/eval_hook.h"
#endif

extern "C" {

int Ci_InitFrameEvalFunc() {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 owns its entry point explicitly: it records the evaluator it
  // replaces and refuses to displace a third party's.
  Ci_SetStaticFunctionVectorcall(Ci_StaticFunction_Vectorcall);
  Ci_EvalFrameFunc = Ci_EvalFrame;
  if (Ci_EvalHook311_Install() < 0) {
    Ci_SetStaticFunctionVectorcall(nullptr);
    Ci_EvalFrameFunc = nullptr;
    return -1;
  }
  return 0;
#else
#ifdef ENABLE_INTERPRETER_LOOP
  Ci_SetStaticFunctionVectorcall(Ci_StaticFunction_Vectorcall);
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = Ci_EvalFrame;
#elif defined(ENABLE_PEP523_HOOK)
  // Let borrowed.h know the eval frame pointer
  Ci_EvalFrameFunc = Ci_EvalFrame;

  auto interp = PyInterpreterState_Get();
  auto current_eval_frame = _PyInterpreterState_GetEvalFrameFunc(interp);
  if (current_eval_frame == Ci_EvalFrame) {
    return 0;
  }
  if (current_eval_frame != _PyEval_EvalFrameDefault) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "CinderX tried to set a frame evaluator function but something else "
        "has done it first, this is not supported");
    return -1;
  }

  _PyInterpreterState_SetEvalFrameFunc(interp, Ci_EvalFrame);
#if PY_VERSION_HEX >= 0x030F0000
  _PyInterpreterState_SetEvalFrameAllowSpecialization(interp, 1);
#endif
#endif
#endif

  return 0;
#endif // PY_VERSION_HEX < 0x030C0000
}

void Ci_FiniFrameEvalFunc() {
#if PY_VERSION_HEX < 0x030C0000
  // Restores the evaluator we replaced; leaves a third party's in place if it
  // took over while we were installed.
  Ci_EvalHook311_Remove();
  Ci_SetStaticFunctionVectorcall(nullptr);
  Ci_EvalFrameFunc = nullptr;
#else
#ifdef ENABLE_INTERPRETER_LOOP
  Ci_SetStaticFunctionVectorcall(nullptr);
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = nullptr;
#elif defined(ENABLE_PEP523_HOOK)
  _PyInterpreterState_SetEvalFrameFunc(PyInterpreterState_Get(), nullptr);
#endif
#endif
#endif // PY_VERSION_HEX < 0x030C0000
}

} // extern "C"
