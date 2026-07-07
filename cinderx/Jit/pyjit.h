// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit_result.h"

namespace jit {

/*
 * This defines the global public API for the JIT that is consumed by the
 * runtime.
 *
 * These methods assume that the GIL is held unless it is explicitly stated
 * otherwise.
 */

/*
 * Initialize any global state required by the JIT.
 *
 * This must be called before attempting to use the JIT.
 *
 * Returns 0 on success, -1 on error, or -2 if we just printed the JIT args.
 */
int initialize();

/*
 * Clean up any resources allocated by the JIT.
 *
 * This is intended to be called at interpreter shutdown in Py_Finalize().
 */
void finalize();

/*
 * Overwrite the entry point of a function so that it tries to JIT-compile
 * itself in the future.
 *
 * By default this will trigger the JIT the next time the function is called,
 * unless AutoJIT is enabled, in that case the function will compile after it is
 * called more times than the AutoJIT threshold.  Before that it will run
 * through the interpreter.
 *
 * Return true if the function was successfully scheduled for compilation, or if
 * it is already compiled.
 */
bool scheduleJitCompile(BorrowedRef<PyFunctionObject> func);

void recordDeoptForRoiBackoff(
    CodeRuntime* code_runtime,
    DeoptReason reason,
    bool is_instrumentation_deopt);

// 守卫自适应去特化：kGuardFailure 深度 deopt 按 code 计数，越限即
// 卸载并置粘滞位，[P3] 计数已越阈使其在下次调用以去特化输入重编
//（specializedOpcode() 消费点见 bytecode.cpp）。
void recordDeoptForDespec(
    CodeRuntime* code_runtime,
    DeoptReason reason,
    bool is_instrumentation_deopt);

bool roiBackoffAllowsCompile(BorrowedRef<PyCodeObject> code);

/*
 * JIT compile func and patch its entry point.
 *
 * On success, positional only calls to func will use the JIT compiled version.
 */
Result compileFunction(BorrowedRef<PyFunctionObject> func);

void uncompile(BorrowedRef<PyFunctionObject> func);

/*
 * Preload a function, along with any functions that it calls that we might want
 * to compile afterwards as well.  This is to support inlining and faster
 * invokes for Static Python functions.
 *
 * Setting the `forcePreload` will bypass the "might want to compile" logic and
 * force all the preloads to happen unconditionally.
 *
 * Return a list of preloaders that were created.  There should be at least one
 * preloader in the list, if it's empty then there was a preloading failure.
 */
std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func,
    bool forcePreload = false);

/*
 * Inform the JIT that a code, function, or type object is being modified or
 * destroyed.
 */
void codeDestroyed(BorrowedRef<PyCodeObject> code);
void funcDestroyed(BorrowedRef<PyFunctionObject> func);
void funcModified(BorrowedRef<PyFunctionObject> func);

#if PY_VERSION_HEX < 0x030C0000
// 3.11 无 function watcher（PyFunction_EVENT_DESTROY 为 3.12+），
// funcDestroyed 在 3.11 下无人触发：已编译函数对象死亡后注册表悬垂，
// finalize() 对尸体写 vectorcall（异步负载在 auto>2 阈值实测 SIGSEGV；
// auto=2 下为静默越界写）。以 weakref 回调复刻 watcher：CPython 析构
// 顺序中 PyObject_ClearWeakRefs 先于字段清理，回调期对象字段完整，
// 可安全走完整 funcDestroyed 注销链。编译注册时布防。
void watchFuncDeath311(BorrowedRef<PyFunctionObject> func);
void clearFuncDeathWatches311();
#endif
void typeDestroyed(BorrowedRef<PyTypeObject> type);
void typeModified(BorrowedRef<PyTypeObject> type);
void typeNameModified(BorrowedRef<PyTypeObject> type);

// Exposed for unit tests
Result compilePreloaderImpl(
    jit::CompilerContext<Compiler>* jit_ctx,
    const hir::Preloader& preloader,
    BorrowedRef<PyFunctionObject> func);

} // namespace jit
