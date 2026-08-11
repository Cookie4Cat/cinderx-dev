// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 opcode tables for shared CinderX code.  Unlike later versions
// this port generates no opcode metadata of its own: the tables come straight
// from the target interpreter's pycore_opcode.h, instantiated in C by
// cinder_opcode_tables.c because their C99 designated initializers do not
// compile as C++.  Consumers see only these declarations.

#include "cinderx/python.h"

// Opcode number macros (SETUP_FINALLY, END_ASYNC_FOR, ...).  Later versions
// get these from the generated cinder_opcode_ids.h; on 3.11 the stock header
// is the single source of truth.
#include "opcode.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t _CiOpcode_Caches[256];
extern const uint8_t _CiOpcode_Deopt[256];

#ifdef __cplusplus
}
#endif
