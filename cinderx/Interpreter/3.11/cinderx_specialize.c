// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Vendored PEP 659 specializer for CPython 3.11 (design decision D3, M2
// "保留 quickening"). ceval/specialize.c is byte-identical to upstream
// v3.11.6 and hash-locked; deviations live here.
//
// Rationale: the manylinux/static-libpython runtime does not export the
// _Py_Specialize_* family, _PyOpcode_Adaptive, or _PyCode_Quicken, and the
// vendored eval loop must specialize exactly like the stock loop or the
// bytecode it leaves behind diverges from stock (config-② equivalence).

// [P1] This TU owns the opcode tables (_PyOpcode_Deopt/_PyOpcode_Jump/...)
// that pycore_opcode.h only defines for the TU that requests them. Upstream
// uses the same mechanism; we merely choose the defining TU.
#define NEED_OPCODE_TABLES

#include "ceval/specialize.c"
