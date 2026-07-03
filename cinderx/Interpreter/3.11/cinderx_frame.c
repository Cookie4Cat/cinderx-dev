// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Vendored interpreter-frame helpers for CPython 3.11 (design decision D3).
// ceval/frame.c is byte-identical to upstream v3.11.6 and hash-locked.
//
// Rationale: the manylinux/static-libpython runtime does not export
// _PyFrame_Push/_PyFrame_Copy/_PyFrame_Clear/_PyInterpreterFrame_GetLine,
// which the vendored eval loop calls.

#include "ceval/frame.c"
