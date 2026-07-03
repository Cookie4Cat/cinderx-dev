// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Vendored-interpreter wrapper TU for CPython 3.11 (design decision D3).
//
// Everything under ceval/ is byte-identical to upstream CPython v3.11.6
// (the openEuler 24.03 anchor per the D7 machine verdict) and is locked by
// the source-hash gate; those files are never edited. Every deviation
// required to build the loop inside CinderX lives HERE, one macro per
// deviation, each with a rationale. This file is the "patch list" that
// review approves.

// [P1] Entry-point rename. CinderX installs its own frame evaluator via
// PEP 523 (Ci_EvalFrame); the vendored loop gets a Ci_ name so calls are
// explicit and cannot be confused with libpython's exported
// _PyEval_EvalFrameDefault. All in-file recursive references follow the
// rename, so once a frame enters the vendored loop it stays in it.
#define _PyEval_EvalFrameDefault Ci_EvalFrameDefault_311

// [P2] PEP 509 dict version counter redirect. The vendored
// STORE_ATTR_INSTANCE_VALUE handler stamps ma_version_tag from
// _pydict_global_version, which static-libpython runtimes (manylinux) do
// not export. Defining our own zero-based counter would alias tags already
// handed out by the runtime's counter and could make version guards see a
// mutated dict as unchanged. Instead the macro redirects to a shadow
// counter that Ci_InitOpcodes seeds to <runtime's current value> + 2^40
// (read back via a probe dict), so the two allocators cannot collide in any
// realistic process lifetime. On the openEuler target (shared libpython,
// symbol exported) the formal port links the real counter directly and
// this patch disappears. (The shadow variable itself is defined in
// cinderx_ceval_shims.c; pycore_dict.h's extern declaration follows the
// rename.)
#define _pydict_global_version ci_pydict_global_version_shadow

#include "ceval/ceval.c"
