// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 JIT scheduling shell.
//
// This SR delivers the buildable/runnable base and the custom interpreter
// loop only.  The JIT source set is therefore not compiled on 3.11 and
// machine-code execution is structurally impossible, not merely gated.  This
// translation unit carries the JIT surface the runtime links against:
// initialization and lifecycle hooks are inert, and compilation entry points
// refuse before any bytecode is read.  Observe mode's scheduling requests
// terminate at Ci_JitShell311_RequestCompile below.

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000

#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Jit/anextawaitable.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/global_cache.h"
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/symbolizer.h"
#include "cinderx/Jit/threaded_compile.h"

namespace jit {

// Runtime configuration. The shell never turns any capability on; the
// defaults in Config already describe a JIT that is off.
Config s_jit_config;

// Type specs the runtime links against for JIT-owned objects. No JIT objects
// exist on 3.11, so these stay empty and are never registered.
PyType_Spec JitGen_Spec{};
PyType_Spec JitCoro_Spec{};
PyType_Spec JitAnextAwaitable_Spec{};
PyTypeObject _JitCoroWrapper_Type{};

int initialize() {
  return 0;
}

void finalize() {}

Context* getContext() {
  return nullptr;
}

void Context::clearDeoptStats() {}

ThreadedCompileContext& getThreadedCompileContext() {
  static ThreadedCompileContext context;
  return context;
}

// Lifecycle notifications from the runtime. Nothing is compiled, so there is
// nothing to invalidate.
void codeDestroyed(BorrowedRef<PyCodeObject>) {}
void funcDestroyed(BorrowedRef<PyFunctionObject>) {}
void funcModified(BorrowedRef<PyFunctionObject>) {}
void typeModified(BorrowedRef<PyTypeObject>) {}

// Nothing is ever scheduled: refusing here is what keeps machine code out of
// reach on 3.11.
bool scheduleJitCompile(BorrowedRef<PyFunctionObject>) {
  return false;
}

void shutdown_jit_genobject_type() {}

// OSR is out of scope on 3.11 and its code-extra slot is never allocated.
void initOSRCodeExtraIndex() {}
void finiOSRCodeExtraIndex() {}

// Auto-JIT phase tracking: no phase is ever entered because nothing schedules
// compilation.
void autoJitImportEnter() {}
void autoJitImportLeave() {}
unsigned autoJitImportDepth() {
  return 0;
}
void autoJitSetupEnter() {}
void autoJitSetupLeave() {}
unsigned autoJitSetupDepth() {
  return 0;
}
unsigned autoJitStartupDepth() {
  return 0;
}

// Symbolizer names JIT frames in profiles. On 3.11 nothing produces them, but
// the type is still linked against, so it resolves nothing.
Symbolizer::Symbolizer(const char*) {}

void Symbolizer::deinit() {}

std::optional<std::string_view> Symbolizer::symbolize(const void*) {
  return std::nullopt;
}

MmapFile::~MmapFile() {}

namespace perf {
bool isPreforkCompilationEnabled() {
  return false;
}
} // namespace perf

} // namespace jit

// The unified compile entry point for this build.  The capability gate fires
// before any bytecode is read: 3.11 ships no machine-code execution, so every
// scheduling request terminates here with the typed refusal.
extern "C" const char* Ci_JitShell311_RequestCompile(PyCodeObject* code) {
  (void)code;
  return CI_OBSERVE_311_REFUSAL;
}

#endif // PY_VERSION_HEX < 0x030C0000
