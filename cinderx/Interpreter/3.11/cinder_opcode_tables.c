// Copyright (c) Meta Platforms, Inc. and affiliates.

// Owner of the CPython 3.11 opcode tables used by shared CinderX code.
//
// The tables in pycore_opcode.h use C99 out-of-order designated initializers,
// which C++ translation units cannot compile, so they are instantiated here in
// C and consumed elsewhere through the declarations in cinder_opcode.h.  The
// _CiOpcode_* renaming keeps this copy distinct from the one the Borrow
// library instantiates for its own translation unit.

#define NEED_OPCODE_TABLES

#define _PyOpcode_Caches _CiOpcode_Caches
#define _PyOpcode_Deopt _CiOpcode_Deopt

#include "cinderx/python.h"

#include "internal/pycore_opcode.h"
