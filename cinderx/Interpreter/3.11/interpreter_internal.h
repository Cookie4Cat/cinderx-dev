// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "Python.h"
#include "internal/pycore_frame.h"

#include "cinderx/Interpreter/3.11/interpreter_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

PyFrameObject* Ci_PyFrame_MakeAndSetFrameObject_311(_PyInterpreterFrame* frame);
PyFrameObject* Ci_PyFrame_New_NoTrack_311(PyCodeObject* code);
void Ci_PyFrame_Copy_311(
    _PyInterpreterFrame* source,
    _PyInterpreterFrame* destination);
void Ci_PyFrame_Clear_311(_PyInterpreterFrame* frame);
_PyInterpreterFrame* Ci_PyFrame_Push_311(
    PyThreadState* tstate,
    PyFunctionObject* function);
int Ci_PyInterpreterFrame_GetLine_311(_PyInterpreterFrame* frame);

#ifdef __cplusplus
}
#endif
