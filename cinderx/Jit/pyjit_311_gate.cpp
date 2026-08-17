// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 observe/shadow compilation gate.  Observe terminates at a
// capability refusal. Shadow runs synchronously under the GIL through
// bytecode, HIR, optimization, LIR, register allocation, target codegen and
// relocation, then discards the artifact without publishing an entry point.

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/module_state.h"

#include <exception>
#include <string_view>

namespace {

const char* functionName(BorrowedRef<PyFunctionObject> func) {
  if (func == nullptr || func->func_qualname == nullptr) {
    return "<unknown>";
  }
  const char* name = PyUnicode_AsUTF8(func->func_qualname);
  return name != nullptr ? name : "<unknown>";
}

constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;

const char* eligibilityReason(BorrowedRef<PyFunctionObject> func) {
  if (!jit::isJitShadow() || cinderx::getModuleState() == nullptr ||
      cinderx::getModuleState()->jit_context == nullptr) {
    return CI_OBSERVE_311_REFUSAL;
  }
  if (func == nullptr || !PyFunction_Check(func)) {
    return "REFUSE_SHAPE_NON_FUNCTION_SCOPE";
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if ((code->co_flags & kRequiredCodeFlags) != kRequiredCodeFlags) {
    return "REFUSE_SHAPE_NON_FUNCTION_SCOPE";
  }
  if (code->co_flags &
      (CO_COROUTINE | CO_ITERABLE_COROUTINE | CO_ASYNC_GENERATOR)) {
    return "REFUSE_SHAPE_ASYNC_CODE";
  }
  if (code->co_flags & CO_GENERATOR) {
    return "REFUSE_SHAPE_GENERATOR_RUNTIME_UNAUDITED";
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return "REFUSE_SHAPE_JIT_SUPPRESSED";
  }
  if (code->co_flags & CI_CO_STATICALLY_COMPILED) {
    return "REFUSE_SHAPE_STATIC_RUNTIME_CACHE";
  }
  if (!PyDict_CheckExact(func->func_globals) ||
      !PyDict_CheckExact(func->func_builtins)) {
    return "REFUSE_SHAPE_NAMESPACE_UNSUPPORTED";
  }

  if (const char* reason = jit::hir::unsupportedShapeReason311(code)) {
    return reason;
  }
  return jit::hir::unsupportedOpcodeReason311(code);
}

} // namespace

extern "C" const char* Ci_JitShell311_RequestCompile(
    PyFunctionObject* raw_func) {
  BorrowedRef<PyFunctionObject> func{raw_func};
  try {
    // Eligibility reads Python objects (co_names, flags, namespaces) and must
    // stay inside the same exception boundary as CompileShadow: a legal
    // CodeType can still raise from the C-API, and that must never escape
    // into the interpreted call.
    const char* reason = eligibilityReason(func);
    if (reason != nullptr) {
      return reason;
    }

    // Preloaders collect Python-object facts on this GIL-holding thread and
    // are destroyed before returning to the interpreter.
    jit::hir::IsolatedPreloaders isolated_preloaders;
    auto* context = static_cast<jit::CompilerContext<jit::Compiler>*>(
        cinderx::getModuleState()->jit_context.get());
    auto result = context->compiler().CompileShadow(func);
    if (!result.has_value()) {
      PyErr_Clear();
      JIT_LOG("shadow compile returned empty for {}", functionName(func));
      return "SUPPORTED_OPCODE_FAILURE";
    }
    jit::triggerStatsOnShadowCompile(
        result->code_size, result->specialized_opcodes);
    return "compiled";
  } catch (const std::exception& exc) {
    // Shadow compilation is observational: failures are reported through the
    // stable event reason and must never perturb the interpreted call.
    PyErr_Clear();
    JIT_LOG("shadow compile failed for {}: {}", functionName(func), exc.what());
    std::string_view what{exc.what()};
    if (what.find("RelocOffsetOutOfRange") != std::string_view::npos) {
      return "REFUSE_SHAPE_CODEGEN_SPAN";
    }
    if (what.find("REFUSE_SHAPE_INVALID_UTF8_NAME") !=
        std::string_view::npos) {
      return "REFUSE_SHAPE_INVALID_UTF8_NAME";
    }
    return "SUPPORTED_OPCODE_FAILURE";
  } catch (...) {
    PyErr_Clear();
    JIT_LOG(
        "shadow compile failed for {}: unknown exception", functionName(func));
    return "SUPPORTED_OPCODE_FAILURE";
  }
}

#endif // PY_VERSION_HEX < 0x030C0000
