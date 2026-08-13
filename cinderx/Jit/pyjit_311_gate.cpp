// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 JIT capability gate.
//
// The full JIT source set is compiled and linked on 3.11, but machine-code
// execution is not part of this delivery.  jit::initialize() returns before
// initializing anything (see pyjit.cpp), so the runtime never becomes usable
// and every compilation entry point refuses.  This translation unit
// terminates the one remaining path -- observe mode's scheduling requests --
// with the typed refusal the gate suite pins.  An execution-capable delivery
// replaces this body with real eligibility and compilation dispatch; the
// callers stay as they are.

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000

#include "cinderx/Interpreter/3.11/observe.h"

extern "C" const char* Ci_JitShell311_RequestCompile(PyCodeObject* code) {
  (void)code;
  return CI_OBSERVE_311_REFUSAL;
}

#endif // PY_VERSION_HEX < 0x030C0000
