// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 observe/shadow compilation gate.  Observe terminates at a
// capability refusal. Shadow runs synchronously under the GIL through
// bytecode, HIR, optimization, LIR, register allocation, target codegen and
// relocation, then discards the artifact without publishing an entry point.

#include "cinderx/python.h"

#include "cinderx/Interpreter/3.11/eval_hook.h"

#if PY_VERSION_HEX < 0x030C0000

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <cstdint>
#include <exception>
#include <string_view>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace {

const char* functionName(BorrowedRef<PyFunctionObject> func) {
  if (func == nullptr || func->func_qualname == nullptr) {
    return "<unknown>";
  }
  const char* name = PyUnicode_AsUTF8(func->func_qualname);
  return name != nullptr ? name : "<unknown>";
}

constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;

// On 3.11 State::kRunning is reachable only through the canary (MR-04)
// initialization path, so it doubles as the execute-mode predicate.
bool canaryMode() {
  return jit::getConfig().state == jit::State::kRunning;
}

bool unicodeEquals311(BorrowedRef<> value, std::string_view expected) {
  if (value == nullptr || !PyUnicode_Check(value)) {
    return false;
  }
  const char* text = PyUnicode_AsUTF8(value);
  return text != nullptr && std::string_view(text) == expected;
}

// Mirrors pyjit.cpp: compiling importlib or cinderx on 3.11 is ineligible
// in getCompilationEligibility(), but the canary observe gate does not
// go through that function.  Once CALL is on the execute whitelist,
// those helpers compile and the next import dies in _ImportLockContext.
bool isCinderModule311(BorrowedRef<PyFunctionObject> func) {
  return unicodeEquals311(func->func_module, "cinderx");
}

bool isImportlibBootstrap311(BorrowedRef<PyFunctionObject> func) {
  if (unicodeEquals311(func->func_module, "_frozen_importlib") ||
      unicodeEquals311(func->func_module, "_frozen_importlib_external") ||
      unicodeEquals311(func->func_module, "importlib._bootstrap") ||
      unicodeEquals311(func->func_module, "importlib._bootstrap_external")) {
    return true;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  if (code == nullptr || code->co_filename == nullptr ||
      !PyUnicode_Check(code->co_filename)) {
    return false;
  }
  const char* file = PyUnicode_AsUTF8(code->co_filename);
  if (file == nullptr) {
    return false;
  }
  return std::string_view(file).find("importlib._bootstrap") !=
      std::string_view::npos;
}

const char* eligibilityReason(BorrowedRef<PyFunctionObject> func) {
  if ((!jit::isJitShadow() && !canaryMode()) ||
      cinderx::getModuleState() == nullptr ||
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
  if (isCinderModule311(func)) {
    return "REFUSE_SHAPE_CINDER_MODULE";
  }
  if (isImportlibBootstrap311(func)) {
    return "REFUSE_SHAPE_IMPORTLIB_BOOTSTRAP";
  }

  if (const char* reason = jit::hir::unsupportedShapeReason311(code)) {
    return reason;
  }
  return jit::hir::unsupportedOpcodeReason311(code);
}

} // namespace

namespace {

// A refusal reason produced inside the compiler, where the eligibility
// predicate cannot see it: the optimizer's own output is only knowable
// once the passes have run.  Thread-local because compilation is
// per-thread; consumed and cleared by the scheduling gate below.
thread_local const char* t_last_execute_refusal = nullptr;

// Argument binding for defaults, keyword-only parameters and the
// variadic collectors is handled by the generated vectorcall prologue
// (JITRT_CallWithKeywordArgs / JITRT_CallWithIncorrectArgcount).  A
// body made of whitelisted opcodes is enough.
const char* unsupportedArgumentShape311(BorrowedRef<PyFunctionObject>) {
  return nullptr;
}

// One compiled artifact, one owning function -- until MR-05 supplies a
// function-destroy notification on 3.11.  CPython 3.11 has no function
// watchers, and the compatibility shim's PyFunction_AddWatcher registers
// nothing, so nothing removes a dead function from the registries.  With a
// single owner the artifact dies with its function and its own destructor
// cleans both registries; with two owners the artifact survives the first
// death and leaves a borrowed pointer to freed memory behind, which
// cinderjit.get_compiled_functions() then dereferences.
const char* unsupportedSharedArtifact311(BorrowedRef<PyFunctionObject> func) {
  auto* ctx =
      static_cast<jit::Context*>(cinderx::getModuleState()->jit_context.get());
  if (ctx == nullptr) {
    return nullptr;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  // One published artifact per code object -- not per compilation key.  The
  // execute ledger is CodeExtra, and it carries exactly one
  // (artifact, globals, builtins) triple per code object; a second artifact
  // for the same code under a different namespace would be published by
  // overwriting the first owner's triple, leaving that owner registered but
  // never executed and the two ledgers contradicting each other.  Refusing
  // the second publication keeps the code object's ledger single-writer;
  // one code serving several namespaces is MR-05 lifecycle work.
  CodeExtra* extra = codeExtraIfExists(code);
  auto* published = extra == nullptr
      ? nullptr
      : reinterpret_cast<jit::CompiledFunction*>(
            _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (published != nullptr && !published->functions().contains(func)) {
    // The artifact's member set is the ownership oracle, and it lives
    // entirely in C++: a deopt removes the installation but not the
    // membership, so a parked function passes this check on re-enable, and
    // nothing Python code can write -- the artifact reference in
    // func.__dict__ included -- can forge a positive.  A dictionary-based
    // check was tried here and is exactly backwards: that key is ordinary
    // writable function state, so copying it onto a second function forged
    // ownership and reopened the ledger overwrite this refusal exists to
    // prevent.
    return "REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED";
  }
  // Backstop for the same-key second owner while the code-extra cache is
  // not populated: the registry itself may already hold an artifact whose
  // owner is someone else.
  BorrowedRef<jit::CompiledFunction> compiled = ctx->lookupCode(
      code,
      reinterpret_cast<PyDictObject*>(func->func_builtins),
      reinterpret_cast<PyDictObject*>(func->func_globals));
  if (compiled == nullptr) {
    return nullptr;
  }
  for (BorrowedRef<PyFunctionObject> owner : compiled->functions()) {
    if (owner != func) {
      return "REFUSE_SHAPE_SHARED_ARTIFACT_LIFECYCLE";
    }
  }
  return nullptr;
}

// CPython 3.14's c_stack_soft_limit is a PyThreadStateImpl field this
// 3.11 tstate does not have.  Cache the same geometry per thread: hard
// limit one margin above the OS stack base, soft limit two margins (stacks
// grow down).
constexpr std::uintptr_t kCStackMarginBytes311 = 16 * 1024;

thread_local std::uintptr_t t_c_stack_soft_limit = 0;
thread_local std::uintptr_t t_c_stack_hard_limit = 0;

std::uintptr_t machineStackPointer311() {
  volatile char here;
  return reinterpret_cast<std::uintptr_t>(&here);
}

void initCStackLimits311() {
  if (t_c_stack_soft_limit != 0) {
    return;
  }
#if defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* stackaddr = nullptr;
    size_t stacksize = 0;
    if (pthread_attr_getstack(&attr, &stackaddr, &stacksize) == 0 &&
        stackaddr != nullptr && stacksize > 3 * kCStackMarginBytes311) {
      auto base = reinterpret_cast<std::uintptr_t>(stackaddr);
      t_c_stack_hard_limit = base + kCStackMarginBytes311;
      t_c_stack_soft_limit = base + 2 * kCStackMarginBytes311;
      pthread_attr_destroy(&attr);
      return;
    }
    pthread_attr_destroy(&attr);
  }
#endif
  std::uintptr_t here = machineStackPointer311();
  constexpr std::uintptr_t kFallbackBudget = 1024 * 1024;
  if (here > kFallbackBudget + 2 * kCStackMarginBytes311) {
    t_c_stack_soft_limit = here - kFallbackBudget;
    t_c_stack_hard_limit = t_c_stack_soft_limit - kCStackMarginBytes311;
  } else {
    t_c_stack_soft_limit = 1;
    t_c_stack_hard_limit = 1;
  }
}

bool cStackSoftLimitReached311() {
  initCStackLimits311();
  std::uintptr_t here = machineStackPointer311();
  if (here < t_c_stack_hard_limit) {
    Py_FatalError("Unrecoverable stack overflow");
  }
  return here < t_c_stack_soft_limit;
}

} // namespace

// The strict MR-04 execute surface: everything the shadow eligibility
// rejects, plus the argument shape, the artifact ownership rule and the
// machine-code whitelist scan.  Exported so the single compile-and-install
// choke point in compileFunction() enforces the same surface no matter
// which door a request came through.
extern "C" const char* Ci_JitShell311_ExecuteRefusal(
    PyFunctionObject* raw_func) {
  BorrowedRef<PyFunctionObject> func{raw_func};
  const char* reason = eligibilityReason(func);
  if (reason != nullptr) {
    return reason;
  }
  if (const char* arg_reason = unsupportedArgumentShape311(func)) {
    return arg_reason;
  }
  if (const char* share_reason = unsupportedSharedArtifact311(func)) {
    return share_reason;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  return jit::hir::unsupportedExecuteReason311(code);
}

extern "C" void Ci_JitShell311_SetExecuteRefusal(const char* reason) {
  t_last_execute_refusal = reason;
}

// Read and clear.  Every consumer takes the reason exactly once, so a
// refusal cannot be attributed to a later, unrelated compile.
extern "C" const char* Ci_JitShell311_TakeExecuteRefusal(void) {
  const char* reason = t_last_execute_refusal;
  t_last_execute_refusal = nullptr;
  return reason;
}

// The entry point installed on 3.11 canary functions.
//
// CPython 3.11 has no function watchers, so nothing tells the runtime when
// a function's code object changes after it was compiled: the compatibility
// shim's PyFunction_AddWatcher registers nothing.  Machine code that
// assumed the old code object would keep running against the new one.
// Defaults and keyword names are rebound on every call by the generated
// prologue, so they do not force a fallback here.
//
// So the entry re-checks, on every call, what compilation assumed, and
// hands anything else back to the interpreter.  The artifact is looked up
// through the current code object rather than captured, so a code swap
// routes to that code's own artifact or to the interpreter, never to a
// stale one.
// The function-state half of the entry's decision, shared with
// isJitCompiled() so "is this compiled?" and "will this call run machine
// code?" cannot answer differently.  Tracing and profiling are thread
// state, not function state: they fall back in the entry without clearing
// the artifact.
extern "C" void* Ci_JitShell311_InstalledArtifact(PyFunctionObject* func) {
  // Fail closed when the frame-evaluator entry point is no longer ours.
  // Initialization installed and verified it, but the slot can change
  // hands afterwards: remove_frame_evaluator() stays published for
  // testing, and another PEP 523 client can take the slot outright.
  // Without the evaluator, the interpreter's specialized CALL pushes
  // callee frames inline and never consults the vectorcall entry, so
  // answering "installed" here would call a function compiled for
  // exactly the calls that bypass its machine code.  Nothing is torn
  // down: reinstalling the evaluator makes the same published artifact
  // serve again.
  if (!Ci_EvalHook311_IsInstalled()) {
    return nullptr;
  }
  // Fail closed under legacy tracing.  MR-04 implements no instrumentation,
  // and machine code delivers none of the call, line and return events a
  // registered trace or profile function is owed -- so while either is
  // active on this thread, nothing may run compiled.  The criterion is the
  // same one hasActiveLegacyTracing() applies on this branch; it is read
  // directly because the answer has to be per-call and per-thread.
  PyThreadState* tstate = PyThreadState_Get();
  if (tstate == nullptr || tstate->c_tracefunc != nullptr ||
      tstate->c_profilefunc != nullptr) {
    return nullptr;
  }
  // With membership persisting across a deopt, the ledger alone no longer
  // distinguishes installed from parked; the entry point does.  A parked
  // function's vectorcall is the interpreter's, so the honest answer to
  // "will this call run machine code" is null until re-enable installs the
  // guarded entry again.
  if (func->vectorcall !=
      reinterpret_cast<vectorcallfunc>(Ci_JitShell311_GuardedEntry)) {
    return nullptr;
  }
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  CodeExtra* extra = codeExtraIfExists(code);
  jit::CompiledFunction* compiled = extra == nullptr
      ? nullptr
      : reinterpret_cast<jit::CompiledFunction*>(
            _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (compiled == nullptr || extra->jit_globals != func->func_globals ||
      extra->jit_builtins != func->func_builtins ||
      // The artifact must be this function's own.
      !compiled->functions().contains(func)) {
    return nullptr;
  }
  return compiled;
}

extern "C" PyObject* Ci_JitShell311_GuardedEntry(
    PyObject* func_obj,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  auto func = reinterpret_cast<PyFunctionObject*>(func_obj);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  PyThreadState* tstate = PyThreadState_GET();
  if (compiled == nullptr || tstate->c_tracefunc != nullptr ||
      tstate->c_profilefunc != nullptr) {
    return getInterpretedVectorcall(func)(func_obj, args, nargsf, kwnames);
  }
  // Keyword names and a mismatched positional count are the generated
  // prologue's job (JITRT_CallWithKeywordArgs /
  // JITRT_CallWithIncorrectArgcount).  Do not filter them here.
  // Two-layer recursion guard (MR-06).  3.11 eval enters at start_frame;
  // the canary vectorcall never goes through eval, so the entry does it.
  // C-stack first so a soft-limit hit does not mutate recursion_remaining.
  if (cStackSoftLimitReached311()) {
    PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded");
    return nullptr;
  }
  if (Py_EnterRecursiveCall("")) {
    return nullptr;
  }
  // Pin the artifact for the duration of the call.  The body runs
  // arbitrary Python through operators and iteration, and that code can
  // drop the artifact's last reference -- clearing the function's __dict__
  // is a documented way to uncompile -- which would free the very code
  // buffer being executed.  The reference is held until machine code has
  // returned.
  Ref<jit::CompiledFunction> pin{Ref<jit::CompiledFunction>::create(compiled)};
  PyObject* result =
      compiled->vectorcallEntry()(func_obj, args, nargsf, kwnames);
  Py_LeaveRecursiveCall();
  return result;
}

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

    if (canaryMode()) {
      // canary: compile, install and let the next call execute machine code.
      // The execute surface is narrower than the shadow surface; refusals
      // report their registered reason so observe accounting stays closed.
      // Ask the same predicate the compile choke point uses, so a refusal
      // is reported with its registered reason instead of surfacing later as
      // an opaque compile failure.
      if (const char* execute_reason = Ci_JitShell311_ExecuteRefusal(func)) {
        return execute_reason;
      }
      if (isJitCompiled(func)) {
        return "installed";
      }
      try {
        jit::Result result = jit::compileFunction(func);
        if (result == jit::Result::OK) {
          return "installed";
        }
        PyErr_Clear();
        if (const char* compiler_reason = Ci_JitShell311_TakeExecuteRefusal()) {
          return compiler_reason;
        }
        JIT_LOG(
            "canary compile returned {} for {}",
            static_cast<int>(result),
            functionName(func));
        return "SUPPORTED_OPCODE_FAILURE";
      } catch (const std::exception& exc) {
        PyErr_Clear();
        JIT_LOG(
            "canary compile failed for {}: {}", functionName(func), exc.what());
        return "SUPPORTED_OPCODE_FAILURE";
      } catch (...) {
        PyErr_Clear();
        JIT_LOG(
            "canary compile failed for {}: unknown exception",
            functionName(func));
        return "SUPPORTED_OPCODE_FAILURE";
      }
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
    if (what.find("REFUSE_SHAPE_INVALID_UTF8_NAME") != std::string_view::npos) {
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
