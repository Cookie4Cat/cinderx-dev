// Copyright (c) Meta Platforms, Inc. and affiliates.

// This TU is the complete patch surface for upstream/frame.c. The included
// file is hash-locked and remains byte-identical to the Release 34 rpmbuild
// source. Renames isolate the vendored implementation from libpython.

#define Py_BUILD_CORE

#define _PyFrame_Traverse Ci_PyFrame_Traverse_311
#define _PyFrame_New_NoTrack Ci_PyFrame_New_NoTrack_311
#define _PyFrame_MakeAndSetFrameObject Ci_PyFrame_MakeAndSetFrameObject_311
#define _PyFrame_Copy Ci_PyFrame_Copy_311
#define _PyFrame_Clear Ci_PyFrame_Clear_311
#define _PyFrame_Push Ci_PyFrame_Push_311
#define _PyInterpreterFrame_GetLine Ci_PyInterpreterFrame_GetLine_311

#include "upstream/frame.c"
