// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Stock CPython 3.11 support currently keeps ENABLE_INTERPRETER_LOOP disabled,
// so there is no CinderX dispatch table for this version. The file exists so
// the common CMake header-copy path can treat 3.11 like other supported
// versions.
