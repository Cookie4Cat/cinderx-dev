// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <iostream>

extern "C" {

bool isJitCompiled(const PyFunctionObject* func) {
  // Possible that this is called during finalization after the module state is
  // destroyed.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return false;
  }
  jit::ICodeAllocator* code_allocator = mod_state->code_allocator.get();
  if (code_allocator == nullptr) {
    return false;
  }
  if (code_allocator->contains(
          reinterpret_cast<const void*>(func->vectorcall))) {
    return true;
  }
  // 3.11：编译函数的 vectorcall 槽位安装的是递归守卫包装（context.cpp），
  // 指针不落在代码池内，须回退到按上下文查询编译状态。
  if (jit::isRecursionGuardVectorcall(func->vectorcall)) {
    jit::Context* ctx = jit::getContext();
    return ctx != nullptr &&
        ctx->lookupFunc(const_cast<PyFunctionObject*>(func)) != nullptr;
  }
  return false;
}

} // extern "C"

namespace jit {

// The key used to store the CompiledFunction in a function's __dict__.
PyObject *kCompiledFunctionKey, *kNestedCompiledFunctionsKey = nullptr;

namespace {

void compiledfunc_dealloc(PyObject* self) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  PyObject_GC_UnTrack(self);

  // Call destructor for C++ members.
  cf->~CompiledFunction();

  Py_TYPE(self)->tp_free(self);
}

int compiledfunc_traverse(PyObject* self, visitproc visit, void* arg) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  return cf->traverse(visit, arg);
}

int compiledfunc_clear(PyObject* self) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  cf->clear();
  return 0;
}

static PyTypeObject _CiCompiledFunction_Type = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name = "CompiledFunction",
    .tp_basicsize = sizeof(jit::CompiledFunction),
    .tp_dealloc = jit::compiledfunc_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = jit::compiledfunc_traverse,
    .tp_clear = jit::compiledfunc_clear,
};

} // namespace

} // namespace jit

namespace jit {

int initCompiledFunctionType() {
  if (PyType_Ready(&_CiCompiledFunction_Type) < 0) {
    return -1;
  }

  // Create the key used to store CompiledFunction in function's __dict__.
  kCompiledFunctionKey =
      PyUnicode_InternFromString("__cinderx_compiled_func__");
  if (kCompiledFunctionKey == nullptr) {
    return -1;
  }

  kNestedCompiledFunctionsKey =
      PyUnicode_InternFromString("__cinderx_nested_compiled_funcs__");
  if (kNestedCompiledFunctionsKey == nullptr) {
    return -1;
  }

  return 0;
}

BorrowedRef<PyTypeObject> getCompiledFunctionType() {
  return &_CiCompiledFunction_Type;
}

Ref<CompiledFunction> CompiledFunction::create(
    CompiledFunctionData&& compiled_func,
    bool immortal) {
  // Allocate the Python object.
  CompiledFunction* cf;
  if (!immortal) {
    cf = PyObject_GC_New(CompiledFunction, &_CiCompiledFunction_Type);
    if (cf == nullptr) {
      return nullptr;
    }
    // Use placement new to construct C++ members in-place.
    new (cf) CompiledFunction(std::move(compiled_func));
    PyObject_GC_Track(reinterpret_cast<PyObject*>(cf));
  } else {
    cf = new CompiledFunction(std::move(compiled_func));
    if (cf == nullptr) {
      return nullptr;
    }
    PyObject_Init(&cf->ob_base, &_CiCompiledFunction_Type);
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(&cf->ob_base);
#else
    _Py_SetImmortal(&cf->ob_base);
#endif
  }

  // Return a stolen reference - the caller owns it.
  return Ref<CompiledFunction>::steal(cf);
}

CompiledFunction::~CompiledFunction() {
  clear();

  auto mod_state = cinderx::getModuleState();
  if (mod_state != nullptr && data_.code.data() != nullptr) {
    auto code_allocator = mod_state->code_allocator.get();
    if (code_allocator != nullptr) {
      code_allocator->releaseCode(const_cast<std::byte*>(data_.code.data()));
    }
  }
}

bool associateFunctionWithCompiled(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled,
    bool is_nested) {
  if (_Py_IsImmortal(func)) {
    // The function can never be freed, so we can keep the CompiledFunction
    // alive
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(compiled);
#else
    _Py_SetImmortal(compiled);
#endif
    return true;
  } else if (_Py_IsImmortal(compiled)) {
    // The CompiledFunction is already immortal, no need to associate it
    return true;
  }
  // Store a reference to the CompiledFunction in the function's __dict__.
  JIT_DCHECK(
      kCompiledFunctionKey != nullptr && kNestedCompiledFunctionsKey != nullptr,
      "failed to create compilation key");
  PyObject* func_dict = func->func_dict;
  if (func_dict == nullptr) {
    func_dict = PyDict_New();
    if (func_dict == nullptr) {
      return false;
    }
    func->func_dict = func_dict;
  }

  if (is_nested) {
    // Store CompiledFunction for nested function in outer function, we'll
    // also store it in the nested function's __dict__ when we do a
    // finalizeFunc on it.
    Ref<> nested_list = getDictRef(func_dict, kNestedCompiledFunctionsKey);
    if (nested_list == nullptr || !PyList_CheckExact(nested_list)) {
      if (PyErr_Occurred()) {
        return false;
      }
      nested_list = Ref<>::steal(PyList_New(0));
      if (nested_list == nullptr) {
        return false;
      } else if (
          PyDict_SetItem(func_dict, kNestedCompiledFunctionsKey, nested_list) <
          0) {
        return false;
      }
    }
    return PyList_Append(nested_list, compiled) == 0;
  }
  // Store the reference (this increfs the CompiledFunction).
  if (PyDict_SetItem(
          func_dict,
          kCompiledFunctionKey,
          reinterpret_cast<PyObject*>(compiled.get())) < 0) {
    return false;
  }

  // Add the function to the CompiledFunction's set.
  compiled->addFunction(func);

  return true;
}

void CompiledFunction::disassemble() const {
  auto start = reinterpret_cast<const char*>(codeBuffer().data());
  Disassembler dis{start, codeSize()};
  dis.disassembleAll(std::cout);
}

void CompiledFunction::printHIR() const {
  JIT_CHECK(
      data_.irfunc != nullptr,
      "Can only call CompiledFunction::printHIR() from a debug build");
  jit::hir::HIRPrinter printer;
  printer.Print(std::cout, *data_.irfunc);
}

std::chrono::nanoseconds CompiledFunction::compileTime() const {
  return data_.compile_time;
}

void CompiledFunction::setCompileTime(std::chrono::nanoseconds time) {
  data_.compile_time = time;
}

void CompiledFunction::setCodePatchers(
    std::vector<std::unique_ptr<CodePatcher>>&& code_patchers) {
  data_.code_patchers = std::move(code_patchers);
}

void CompiledFunction::setHirFunc(std::unique_ptr<hir::Function>&& irfunc) {
  data_.irfunc = std::move(irfunc);
}

void* CompiledFunction::staticEntry() const {
  if (data_.runtime == nullptr ||
      !(data_.runtime->code()->co_flags & CI_CO_STATICALLY_COMPILED)) {
    return nullptr;
  }

  return reinterpret_cast<void*>(
      JITRT_GET_STATIC_ENTRY(data_.vectorcall_entry));
}

void CompiledFunction::addFunction(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 没有 function watcher（DEALLOC 事件不可得），借用引用无法在
  // 函数死亡时经 funcDestroyed() 摘除，遗留的悬垂指针会在 clear() 的
  // vectorcall 复位循环中造成对已释放函数对象的 UAF 写（M9 预演
  // spectral_norm 案）。3.11 改为持有强引用，并在 traverse 中上报：
  // 函数 __dict__ 中对 CompiledFunction 的强引用与此处构成引用环，
  // 由 GC 统一回收（clear() 即 tp_clear 断环点）。
  if (functions_.insert(func.get()).second) {
    Py_INCREF(func.get());
  }
#else
  // Store a borrowed reference to the function. The function is responsible
  // for removing itself via funcDestroyed() when it is deallocated.
  // We don't incref to avoid preventing garbage collection of functions
  // when multiple functions share the same CompiledFunction.
  functions_.insert(func.get());
#endif
}

void CompiledFunction::removeFunction(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  auto it = functions_.find(func.get());
  if (it != functions_.end()) {
    functions_.erase(it);
    Py_DECREF(func.get());
  }
#else
  // Remove the borrowed reference. No decref needed since we don't own it.
  functions_.erase(func.get());
#endif
}

int CompiledFunction::traverse(visitproc visit, void* arg) {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 持有函数的强引用（见 addFunction），必须上报给 GC 才能回收
  // 函数与 CompiledFunction 之间的引用环。
  for (PyFunctionObject* func : functions_) {
    Py_VISIT(reinterpret_cast<PyObject*>(func));
  }
#else
  // Don't traverse functions_ - these are borrowed references that we don't
  // own. The functions are responsible for removing themselves via
  // funcDestroyed() when they are deallocated. Not traversing them allows
  // functions to be garbage collected independently of this CompiledFunction.
#endif

  // Traverse all references held by the CodeRuntime.
  if (data_.runtime != nullptr) {
    return data_.runtime->traverse(visit, arg);
  }

  return 0;
}

void CompiledFunction::clear(bool context_finalizing) {
  // Copy function pointers before clearing the set.
  if (owner_ != nullptr) {
    if (!context_finalizing) {
      // These will be cleared on their own if we're finaling the context,
      // let's not complicate the iteration.
      owner_->forgetCompiledFunction(*this);
    }
    for (auto& patcher : data_.code_patchers) {
      if (auto typed_patcher = dynamic_cast<TypeDeoptPatcher*>(patcher.get())) {
        owner_->unwatch(typed_patcher);
      }
    }

    // We only de-opt the functions if we still have our owner. This is so that
    // our multi-threaded compile tests work properly where we are clearing
    // things with functions still running.
    auto funcs_to_deopt = std::move(functions_);

#if PY_VERSION_HEX < 0x030C0000
    // 3.11 持有强引用：函数对象此刻必然存活，但可能已经历 GC 的
    // tp_clear（func_code 已清空），不得读取 func_code 决定入口；3.11
    // 不支持 Static Python，解释器入口恒为 Ci_PyFunction_Vectorcall。
    // 复位后归还引用（与 addFunction 的 incref 配对），并清理 Context
    // 中以函数指针为键的簿记（3.14 由 func watcher 在函数死亡时完成）。
    for (PyFunctionObject* func : funcs_to_deopt) {
      func->vectorcall = Ci_PyFunction_Vectorcall;
      Py_DECREF(reinterpret_cast<PyObject*>(func));
    }
#else
    // Deopt all associated functions. No decref needed since these are borrowed
    // refs.
    for (PyFunctionObject* func : funcs_to_deopt) {
      func->vectorcall = getInterpretedVectorcall(func);
    }
#endif

    owner_ = nullptr;
  }

  // Clear all references held by the CodeRuntime.
  if (data_.runtime != nullptr) {
    data_.runtime->releaseReferences();
    data_.runtime = nullptr;
  }
}

} // namespace jit
