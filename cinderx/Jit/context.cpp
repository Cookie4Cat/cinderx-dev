// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "internal/pycore_interp.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"


#ifndef WIN32
#include <dlfcn.h>
#include <sys/mman.h>
#endif

namespace jit {

AotContext g_aot_ctx;

std::recursive_mutex& freeThreadedJITEntrypointMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point) {
  return yield_point->isYieldFrom()
      ? reinterpret_cast<PyObject*>(
            *(reinterpret_cast<uint64_t*>(gen_footer) +
              yield_point->yieldFromOffset()))
      : nullptr;
}

void Builtins::init() {
  ThreadedCompileSerialize guard;
  if (is_initialized_) {
    return;
  }
  // we want to check the exact function address, rather than relying on
  // modules which can be mutated.  First find builtins, which we have
  // to do a search for because PyEval_GetBuiltins() returns the
  // module dict.
  PyObject* mods =
      CI_INTERP_IMPORT_FIELD(_PyInterpreterState_GET(), modules_by_index);
  PyModuleDef* builtins = nullptr;
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(mods); i++) {
    PyObject* cur = PyList_GET_ITEM(mods, i);
    if (cur == Py_None) {
      continue;
    }
    PyModuleDef* def = PyModule_GetDef(cur);
    if (def == nullptr) {
      PyErr_Clear();
      continue;
    }
    if (std::strcmp(def->m_name, "builtins") == 0) {
      builtins = def;
      break;
    }
  }
  JIT_CHECK(builtins != nullptr, "could not find builtins module");

  auto add = [this](const std::string& name, PyMethodDef* meth) {
    cfunc_to_name_[meth] = name;
    name_to_cfunc_[name] = meth;
  };
  // Find all free functions.
  for (PyMethodDef* fdef = builtins->m_methods; fdef->ml_name != nullptr;
       fdef++) {
    add(fdef->ml_name, fdef);
  }
  // Find all methods on types.
  PyTypeObject* types[] = {
      &PyDict_Type,
      &PyList_Type,
      &PyTuple_Type,
      &PyUnicode_Type,
  };
  for (auto type : types) {
    for (PyMethodDef* fdef = type->tp_methods; fdef->ml_name != nullptr;
         fdef++) {
      add(fmt::format("{}.{}", type->tp_name, fdef->ml_name), fdef);
    }
  }
  // Only mark as initialized after everything is done to avoid concurrent
  // reads of an unfinished map.
  is_initialized_ = true;
}

bool Builtins::isInitialized() const {
  return is_initialized_;
}

std::optional<std::string> Builtins::find(PyMethodDef* meth) const {
  auto result = cfunc_to_name_.find(meth);
  if (result == cfunc_to_name_.end()) {
    return std::nullopt;
  }
  return result->second;
}

std::optional<PyMethodDef*> Builtins::find(const std::string& name) const {
  auto result = name_to_cfunc_.find(name);
  if (result == name_to_cfunc_.end()) {
    return std::nullopt;
  }
  return result->second;
}

Context::Context()
    : zero_(Ref<>::steal(PyLong_FromLong(0))),
      str_build_class_(Ref<>::create(&_Py_ID(__build_class__))) {
#if PY_VERSION_HEX >= 0x030E0000
  for (int i = 0; i < NUM_COMMON_CONSTANTS; i++) {
    JIT_CHECK(Ci_common_consts[i] != nullptr, "common_consts[{}] is null", i);
    common_constant_types_.emplace_back(
        hir::Type::fromObject(Ci_common_consts[i]));
  }
#endif
}

Context::~Context() {
  // Clear all of the CompiledFunction's before we clear out the memory used for
  // the CodeRuntime allocated in the slab.
  for (auto& code : compiled_codes_) {
    code.second->clear(true /* context_finalizing */);
  }
}

void Context::mlockProfilerDependencies() {
#ifndef WIN32
  for (auto& codert : code_runtimes_) {
    if (codert.isCleared()) {
      continue;
    }
    PyCodeObject* code = codert.code().get();
    if (code == nullptr) {
      continue;
    }
    ::mlock(code, sizeof(PyCodeObject));
    ::mlock(code->co_qualname, Py_SIZE(code->co_qualname));
  }
  code_runtimes_.mlock();
#endif
}

Ref<> Context::pageInProfilerDependencies() {
  ThreadedCompileSerialize guard;
  Ref<> qualnames = Ref<>::steal(PyList_New(0));
  if (qualnames == nullptr) {
    return nullptr;
  }
  // We want to force the OS to page in the memory on the
  // code_rt->code->qualname path and keep the compiler from optimizing away
  // the code to do so. There are probably more efficient ways of doing this
  // but perf isn't a major concern.
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    BorrowedRef<> qualname = code_rt.code()->co_qualname;
    if (qualname == nullptr) {
      continue;
    }
    if (PyList_Append(qualnames, qualname) < 0) {
      return nullptr;
    }
  }
  return qualnames;
}

void** Context::findFunctionEntryCache(PyFunctionObject* function) {
  auto result = function_entry_caches_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(function),
      std::forward_as_tuple());
  if (result.second) {
    result.first->second.ptr = pointer_caches_.allocate();
    // _PyClassLoader_HasPrimitiveArgs doesn't work well in multi-threaded
    // compile in 3.12+ due to access of a dictionary with non-key strings.
    // We fix this up post-compile in the multi-threaded case.
    if (!getThreadedCompileContext().compileRunning() &&
        _PyClassLoader_HasPrimitiveArgs((PyCodeObject*)function->func_code)) {
      result.first->second.arg_info =
          Ref<_PyTypedArgsInfo>::steal(_PyClassLoader_GetTypedArgsInfo(
              (PyCodeObject*)function->func_code, 1));
    }
  }
  return result.first->second.ptr;
}

void Context::clearFunctionEntryCache(BorrowedRef<PyFunctionObject> function) {
  function_entry_caches_.erase(function);
}

// See comments in findFunctionEntryCache.
void Context::fixupFunctionEntryCachePostMultiThreadedCompile() {
  for (auto& entry : function_entry_caches_) {
    BorrowedRef<PyCodeObject> code{entry.first->func_code};
    if (entry.second.arg_info.get() == nullptr &&
        _PyClassLoader_HasPrimitiveArgs(code)) {
      entry.second.arg_info = Ref<_PyTypedArgsInfo>::steal(
          _PyClassLoader_GetTypedArgsInfo(code, 1));
    }
  }
}

bool Context::hasFunctionEntryCache(PyFunctionObject* function) const {
  return function_entry_caches_.find(function) != function_entry_caches_.end();
}

_PyTypedArgsInfo* Context::findFunctionPrimitiveArgInfo(
    PyFunctionObject* function) {
  auto cache = function_entry_caches_.find(function);
  if (cache == function_entry_caches_.end()) {
    return nullptr;
  }
  return cache->second.arg_info.get();
}

void Context::recordDeopt(
    CodeRuntime* code_runtime,
    std::size_t idx,
    BorrowedRef<> guilty_value) {
  withDeoptStatsLock([&]() {
    DeoptStat& stat = deopt_stats_[code_runtime][idx];
    stat.count++;
    if (guilty_value != nullptr) {
      stat.types.recordType(Py_TYPE(guilty_value));
    }
  });
}

const DeoptStat* Context::deoptStat(
    const CodeRuntime* code_runtime,
    std::size_t deopt_idx) const {
  auto map_it = deopt_stats_.find(code_runtime);
  if (map_it == deopt_stats_.end()) {
    return nullptr;
  }
  auto stat_it = map_it->second.find(deopt_idx);
  if (stat_it == map_it->second.end()) {
    return nullptr;
  }
  return &stat_it->second;
}

void Context::clearDeoptStats() {
  withDeoptStatsLock([&]() { deopt_stats_.clear(); });
}

InlineCacheStats Context::getAndClearLoadMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadMethodCached instr was
      // optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

InlineCacheStats Context::getAndClearLoadTypeMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_type_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadTypeMethod instr
      // was optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

void Context::setGuardFailureCallback(Context::GuardFailureCallback cb) {
  guard_failure_callback_ = cb;
}

void Context::guardFailed(const DeoptMetadata& deopt_meta) {
  if (guard_failure_callback_) {
    guard_failure_callback_(deopt_meta);
  }
}

void Context::clearGuardFailureCallback() {
  guard_failure_callback_ = nullptr;
}

void Context::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

void Context::releaseReferences() {
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    code_rt.releaseReferences();
  }
  references_.clear();
  type_deopt_patchers_.clear();
}

LoadAttrCache* Context::allocateLoadAttrCache() {
  return load_attr_caches_.allocate();
}

LoadTypeAttrCache* Context::allocateLoadTypeAttrCache() {
  return load_type_attr_caches_.allocate();
}

LoadMethodCache* Context::allocateLoadMethodCache() {
  return load_method_caches_.allocate();
}

LoadModuleAttrCache* Context::allocateLoadModuleAttrCache() {
  return load_module_attr_caches_.allocate();
}

LoadModuleMethodCache* Context::allocateLoadModuleMethodCache() {
  return load_module_method_caches_.allocate();
}

LoadTypeMethodCache* Context::allocateLoadTypeMethodCache() {
  return load_type_method_caches_.allocate();
}

StoreAttrCache* Context::allocateStoreAttrCache() {
  return store_attr_caches_.allocate();
}

const Builtins& Context::builtins() {
  // Lock-free fast path followed by single-lock slow path during
  // initialization.
  if (!builtins_.isInitialized()) {
    builtins_.init();
  }
  return builtins_;
}

void Context::unwatch(TypeDeoptPatcher* patcher) {
  type_deopt_patchers_[patcher->type()].erase(patcher);
}

void Context::watchType(
    BorrowedRef<PyTypeObject> type,
    TypeDeoptPatcher* patcher) {
  ThreadedCompileSerialize guard;
  type_deopt_patchers_[type].emplace(patcher);
  // We require the interpreter state in order to watch types
  if (getThreadedCompileContext().compileRunning()) {
    pending_watches_.emplace(type);
    return;
  }

  JIT_CHECK(
      cinderx::getModuleState()->watcher_state.watchType(type) == 0,
      "Failed to watch type {}",
      type->tp_name);
}

BorrowedRef<> Context::zero() {
  return zero_.get();
}

BorrowedRef<> Context::strBuildClass() {
  return str_build_class_.get();
}

void Context::watchPendingTypes() {
  for (auto& type : pending_watches_) {
    JIT_CHECK(
        cinderx::getModuleState()->watcher_state.watchType(type) == 0,
        "Failed to watch pending type {}",
        type->tp_name);
  }
  pending_watches_.clear();
}

void Context::notifyTypeModified(
    BorrowedRef<PyTypeObject> lookup_type,
    BorrowedRef<PyTypeObject> new_type) {
  notifyICsTypeChanged(lookup_type);

  ThreadedCompileSerialize guard;
  auto it = type_deopt_patchers_.find(lookup_type);
  if (it == type_deopt_patchers_.end()) {
    return;
  }

  std::unordered_set<TypeDeoptPatcher*> remaining_patchers;
  for (TypeDeoptPatcher* patcher : it->second) {
    if (!patcher->maybePatch(new_type)) {
      remaining_patchers.emplace(patcher);
    }
  }

  if (remaining_patchers.empty()) {
    type_deopt_patchers_.erase(it);
    // don't unwatch type; other watchers may still be watching it
  } else {
    it->second = std::move(remaining_patchers);
  }
}

bool Context::hasCompletedCompile(CompilationKey& key) {
  return completed_compiles_.contains(key);
}

void Context::addDeferredFinalization(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  ThreadedCompileSerialize guard;
  deferred_finalizations_.emplace_back(
      ThreadedRef<PyFunctionObject>::create(func), compiled);
}

void Context::finalizeMultiThreadedCompile() {
  fixupFunctionEntryCachePostMultiThreadedCompile();
  watchPendingTypes();

  for (auto& codes : completed_compiles_) {
    makeCompiledFunction(
        codes.second.second, codes.first, std::move(codes.second.first));
  }
  completed_compiles_.clear();

  for (auto& [func, compiled] : deferred_finalizations_) {
    finalizeFunc(func, compiled);
  }
  deferred_finalizations_.clear();
}

bool Context::finalizeFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  compiled->setOwner(this);

  if (!addCompiledFunc(func, compiled)) {
    // Someone else compiled the function between when our caller checked and
    // called us.
    return true;
  }

  // In case the function had previously been deopted.
  removeDeoptedFunc(func);

  setVectorcall(func, compiled->vectorcallEntry());
  if (hasFunctionEntryCache(func)) {
    void** indirect = findFunctionEntryCache(func);
    *indirect = compiled->staticEntry();
  }

  // Associate the function with the CompiledFunction for GC tracking.
  // This is ultimately what will keep the CompiledFunction alive and
  // keep the PyFunctionObject JITed.
  return associateFunctionWithCompiled(func, compiled, false /* is_nested */);
}

namespace {

constexpr size_t kCodeDedupMaxEntries = 2048;

// A code object is eligible for content-keyed compile reuse when it is
// provably namespace-free: its bytecode never reads or writes globals,
// builtins or module-level names, so the compiled artifact cannot reference
// any globals-coupled cache and is valid under any (globals, builtins) pair.
// Suspendable and Static Python code is excluded to keep per-code runtime
// structures out of scope. These are semantic properties of the bytecode;
// which code objects actually recur is decided by behavioral evidence (see
// noteCompiledFuncDestroyed), not by name or origin heuristics.
bool isDedupEligibleCode(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags &
      (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR |
       CI_CO_STATICALLY_COMPILED)) {
    return false;
  }
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    switch ((*it).opcode()) {
      case LOAD_GLOBAL:
      case STORE_GLOBAL:
      case DELETE_GLOBAL:
      case LOAD_NAME:
      case STORE_NAME:
      case DELETE_NAME:
      case LOAD_FROM_DICT_OR_GLOBALS:
      case LOAD_BUILD_CLASS:
      case IMPORT_NAME:
      case IMPORT_FROM:
        return false;
      default:
        break;
    }
  }
  // Exclude code that creates nested functions: co_consts code objects would
  // be shared through the canonical identity, and nested-function creation
  // semantics under adopted identities are not worth reasoning about for
  // this cohort (the exec-generated helpers this targets have flat bodies).
  BorrowedRef<> consts{code->co_consts};
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(consts.get()); ++i) {
    if (PyCode_Check(PyTuple_GET_ITEM(consts.get(), i))) {
      return false;
    }
  }
  return true;
}

// Cheap structural fingerprint over immutable code-object fields, used to
// prefilter registry probes so the common case (no registered donor with a
// matching shape) costs a hash lookup instead of a marshal. Deliberately
// excludes the bytecode buffer, which is mutated in place by the adaptive
// interpreter. Collisions are resolved by full marshal comparison.
uint64_t codeFingerprint(BorrowedRef<PyCodeObject> code) {
  return combineHash(
      static_cast<size_t>(Py_SIZE(code.get())),
      static_cast<size_t>(code->co_argcount),
      static_cast<size_t>(code->co_nlocalsplus),
      static_cast<size_t>(code->co_stacksize),
      static_cast<size_t>(code->co_flags),
      static_cast<size_t>(PyTuple_GET_SIZE(code->co_consts)),
      static_cast<size_t>(PyTuple_GET_SIZE(code->co_names)));
}

} // namespace

namespace {

// Same-origin gate for content reuse: CPython's code equality deliberately
// ignores filename and line numbers, but sharing artifacts across source
// locations would make tracebacks point at a different origin. Require the
// same origin so a donor is only ever the same logical function.
bool sameCodeOrigin(
    BorrowedRef<PyCodeObject> a,
    BorrowedRef<PyCodeObject> b) {
  if (a->co_firstlineno != b->co_firstlineno) {
    return false;
  }
  int eq = PyObject_RichCompareBool(a->co_filename, b->co_filename, Py_EQ);
  if (eq < 0) {
    PyErr_Clear();
    return false;
  }
  return eq == 1;
}

// Value equality of code objects, as maintained by CPython itself: compares
// bytecode (de-instrumented), constants, names and arities by value, and is
// insensitive to string interning state, which makes it stable across
// generations of recreated code (marshal output is not).
bool sameCodeContent(
    BorrowedRef<PyCodeObject> a,
    BorrowedRef<PyCodeObject> b) {
  int eq = PyObject_RichCompareBool(a.getObj(), b.getObj(), Py_EQ);
  if (eq < 0) {
    PyErr_Clear();
    return false;
  }
  return eq == 1;
}

} // namespace

bool Context::reuseDedupedCompiled(BorrowedRef<PyFunctionObject> func) {
  BorrowedRef<PyCodeObject> code{func->func_code};
  // Fingerprint prefilter: only code whose structural shape matches a
  // recorded entry pays the eligibility scan and value comparison.
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return false;
  }
  if (!isDedupEligibleCode(code)) {
    return false;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.compiled != nullptr && entry.compiled->runtime() == nullptr) {
      // Defense in depth: the artifact was gutted by clear() without going
      // through dropDedupArtifact(). Demote instead of re-installing a dead
      // entrypoint; recompilation can re-promote the entry.
      entry.compiled.reset();
    }
    if (entry.compiled == nullptr || entry.code.get() == code.get() ||
        !sameCodeOrigin(code, entry.code) ||
        !sameCodeContent(code, entry.code)) {
      continue;
    }
    // Canonicalize onto the donor code object so all per-code machinery
    // (compiled_codes_, the CodeExtra fast-attach, call counters) converges
    // on a single identity. Assigning through the attribute keeps
    // function-version and watcher bookkeeping correct.
    if (PyObject_SetAttrString(func, "__code__", entry.code.getObj()) < 0) {
      PyErr_Clear();
      return false;
    }
    JIT_DLOG("dedup: attaching twin {}", funcFullname(func));
    return finalizeFunc(func, entry.compiled);
  }
  return false;
}

void Context::noteCodeCompiled(
    const CompilationKey& key,
    BorrowedRef<CompiledFunction> compiled) {
  if (!getConfig().auto_code_twin_dedup) {
    return;
  }
  BorrowedRef<PyCodeObject> code{key.code};
  if (!isDedupEligibleCode(code)) {
    return;
  }
  uint64_t fingerprint = codeFingerprint(code);
  auto bucket_it = code_dedup_cache_.find(fingerprint);
  if (bucket_it != code_dedup_cache_.end()) {
    for (CodeDedupEntry& entry : bucket_it->second) {
      if (entry.code.get() == code.get()) {
        return;
      }
      if (!sameCodeOrigin(code, entry.code) ||
          !sameCodeContent(code, entry.code)) {
        continue;
      }
      // Second compilation of identical content: that is the recurrence
      // evidence this machinery exists for. This compilation becomes the
      // canonical donor; content-identical code compiled later attaches to
      // its pinned artifact instead of recompiling. Namespace-free code has
      // no globals-coupled caches, so the artifact is valid under any
      // (globals, builtins) pair.
      if (entry.compiled == nullptr) {
        JIT_DLOG("dedup: donor promoted for {}", codeQualname(code));
        entry.code = Ref<PyCodeObject>::create(code);
        entry.compiled = Ref<CompiledFunction>::create(compiled);
      }
      return;
    }
  }
  // First sighting: record the code object so a later compilation of
  // identical content can prove recurrence.
  if (code_dedup_size_ < kCodeDedupMaxEntries) {
    code_dedup_cache_[fingerprint].push_back(
        CodeDedupEntry{Ref<PyCodeObject>::create(code), nullptr});
    code_dedup_size_++;
  }
}

void Context::dropDedupArtifact(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<CompiledFunction> compiled) {
  if (code_dedup_cache_.empty()) {
    return;
  }
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.compiled.get() == compiled.get()) {
      JIT_DLOG("dedup: donor demoted for {}", codeQualname(entry.code));
      entry.compiled.reset();
    }
  }
}

void Context::noteCompiledFuncDestroyed(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  if (!getConfig().auto_code_twin_dedup || Py_IsFinalizing()) {
    return;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.code.get() == code.get()) {
      // The dying function's code is itself the recorded entry; pin its
      // artifact so a later recurrence can attach instead of recompiling.
      if (entry.compiled == nullptr && isDedupEligibleCode(code)) {
        JIT_DLOG("dedup: donor rescued from dying {}", funcFullname(func));
        entry.compiled = Ref<CompiledFunction>::create(compiled);
      }
      return;
    }
  }
}

void Context::codeCompiled(
    BorrowedRef<PyFunctionObject> func,
    CompilationKey& key,
    CompiledFunctionData&& compiled_func) {
  addCompileTime(compiled_func.compile_time);

  if (getThreadedCompileContext().compileRunning()) {
    completed_compiles_.emplace(
        key,
        std::pair(
            std::move(compiled_func),
            ThreadedRef<PyFunctionObject>::create(func)));
    return;
  }

  makeCompiledFunction(func, key, std::move(compiled_func));
}

const hir::Type& Context::typeForCommonConstant([[maybe_unused]] int i) const {
#if PY_VERSION_HEX >= 0x030E0000
  return common_constant_types_.at(i);
#endif
  JIT_ABORT("Common constants are a feature of 3.14+");
}

namespace {
// Publish the compiled entry on the code object's CodeExtra so a newly created
// function with the same code+globals+builtins can skip the compiled_codes_
// hashmap lookup. Stores a *borrowed* pointer -- the CompiledFunction is kept
// alive by the usual anchors (the compiling function's __dict__ / the outer
// function's nested list) and is cleared here before it is freed. The pointer
// is published with release ordering after its globals/builtins so a concurrent
// reader (under the same JIT entrypoint guard) sees a consistent triple.
void cacheCompiledOnCode(const CompilationKey& key, CompiledFunction* compiled) {
  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(key.code));
  if (extra == nullptr) {
    return;
  }
  extra->jit_globals = key.globals;
  extra->jit_builtins = key.builtins;
  _Py_atomic_store_ptr_release(&extra->jit_compiled, compiled);
}

// Clear the cache only if it still points at `compiled`. This must run before
// `compiled` is freed and under JIT entrypoint serialization; the check guards
// against clearing an entry republished earlier in the same serialized flow, not
// against lock-free concurrent publishers.
void clearCachedCompiledIfMatches(
    BorrowedRef<PyCodeObject> code,
    CompiledFunction* compiled) {
  CodeExtra* extra = codeExtra(code);
  if (extra != nullptr &&
      _Py_atomic_load_ptr_relaxed(&extra->jit_compiled) == compiled) {
    _Py_atomic_store_ptr_release(&extra->jit_compiled, nullptr);
    extra->jit_globals = nullptr;
    extra->jit_builtins = nullptr;
  }
}
} // namespace

void Context::forgetCode(BorrowedRef<PyFunctionObject> func) {
  auto it = compiled_codes_.find(CompilationKey{func});
  if (it == compiled_codes_.end()) {
    return;
  }

  // Remove the CF from any outer function's nested compiled functions list.
  // When a nested function is compiled, its CF is stored both in the
  // function's own __dict__ and in the outer function's
  // __cinderx_nested_compiled_funcs__ list. We need to clean up the latter
  // when forgetting the code.
  BorrowedRef<CompiledFunction> cf = it->second;
  BorrowedRef<PyCodeObject> code{it->first.code};
  auto outer_it = code_outer_funcs_.find(code);
  if (outer_it != code_outer_funcs_.end() && outer_it->second != func) {
    BorrowedRef<PyFunctionObject> outer = outer_it->second;
    PyObject* outer_dict = outer->func_dict;
    if (outer_dict != nullptr) {
      Ref<> nested_list = getDictRef(outer_dict, kNestedCompiledFunctionsKey);
      if (nested_list != nullptr && PyList_CheckExact(nested_list.get())) {
        for (Py_ssize_t i = PyList_GET_SIZE(nested_list.get()) - 1; i >= 0;
             i--) {
          if (PyList_GET_ITEM(nested_list.get(), i) ==
              reinterpret_cast<PyObject*>(cf.get())) {
            if (PyList_SetSlice(nested_list.get(), i, i + 1, nullptr) < 0) {
              PyErr_Clear();
            }
            break;
          }
        }
      }
    }
  }

  clearCachedCompiledIfMatches(code, cf.get());
  dropDedupArtifact(code, cf);
  it->second->clear();
  compiled_codes_.erase(CompilationKey{func});
}

void Context::forgetCompiledFunction(CompiledFunction& function) {
  // tp_clear() can reach here from GC without going through a guarded
  // top-level JIT entrypoint, so this path has to take the FT guard itself.
  FreeThreadedJITEntrypointGuard guard;
  if (function.runtime() != nullptr) {
    for (auto pyfunc : function.functions()) {
      compiled_funcs_.erase(pyfunc);
    }
    CompilationKey key{function};
    // Drop the CodeExtra fast-path cache before this CompiledFunction is freed,
    // otherwise the cached (borrowed) pointer would dangle.
    clearCachedCompiledIfMatches(
        reinterpret_cast<PyCodeObject*>(key.code), &function);
    compiled_codes_.erase(key);
  }
}

bool Context::didCompile(BorrowedRef<PyFunctionObject> func) {
  ThreadedCompileSerialize guard;
  return compiled_funcs_.contains(func);
}

BorrowedRef<CompiledFunction> Context::lookupFunc(
    BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

CodeRuntime* Context::lookupCodeRuntime(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* compiled = lookupFunc(func);
  if (compiled == nullptr) {
    if (func->func_dict != nullptr) {
      // For multi-threaded compile tests we clear the compiled codes. This is a
      // super funky thing to do because the functions may actually still be
      // running and we may try and get the code runtime. So here we make a
      // last-ditch effort to try and recover the runtime from the function.
      Ref<> compiled_val = getDictRef(func->func_dict, kCompiledFunctionKey);
      if (compiled_val != nullptr &&
          Py_TYPE(compiled_val) == getCompiledFunctionType()) {
        auto compiled_func =
            reinterpret_cast<CompiledFunction*>(compiled_val.get());
        if (compiled_func->functions().contains(func)) {
          return compiled_func->runtime();
        }
      }
    }
    return nullptr;
  }
  return compiled->runtime();
}

const UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>>&
Context::compiledCodes() const {
  return compiled_codes_;
}

const UnorderedMap<
    BorrowedRef<PyFunctionObject>,
    BorrowedRef<CompiledFunction>>&
Context::compiledFuncs() {
  return compiled_funcs_;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::deoptedFuncs() {
  return deopted_funcs_;
}

void Context::addCompileTime(std::chrono::nanoseconds time) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time);
  total_compile_time_ms_.fetch_add(ms.count(), std::memory_order_relaxed);
}

std::chrono::milliseconds Context::totalCompileTime() const {
  return std::chrono::milliseconds{
      total_compile_time_ms_.load(std::memory_order_relaxed)};
}

void Context::setCinderJitModule(Ref<> mod) {
  cinderjit_module_ = std::move(mod);
}

void Context::clearForMultithreadedCompileTest() {
  for (auto& func_entry : compiled_funcs_) {
    BorrowedRef<CompiledFunction> compiled = func_entry.second;
    if (compiled->runtime() != nullptr) {
      CompilationKey key{*compiled.get()};
      clearCachedCompiledIfMatches(
          reinterpret_cast<PyCodeObject*>(key.code), compiled.get());
    }
    // Disconnect from Context so clear() on eventual destruction won't call
    // back into us (e.g., forgetCompiledFunction, unwatch).
    compiled->setOwner(nullptr);
    // Keep the old CompiledFunction alive via a strong reference.
    orphaned_compiled_codes_.emplace_back(
        Ref<CompiledFunction>::create(compiled));
  }
  compiled_codes_.clear();
  compiled_funcs_.clear();
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  auto it = compiled_funcs_.find(func);
  if (it != compiled_funcs_.end()) {
    noteCompiledFuncDestroyed(func, it->second);
    it->second->removeFunction(func);
    compiled_funcs_.erase(func);
  }
  deopted_funcs_.erase(func);
  // This doesn't modify compiled_codes_, so if this is a nested function it can
  // easily be reopted later.
}

BorrowedRef<CompiledFunction> Context::lookupCode(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  ThreadedCompileSerialize guard;
  auto it = compiled_codes_.find(CompilationKey{code, builtins, globals});
  return it == compiled_codes_.end() ? nullptr : it->second.get();
}

void Context::addDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
  deopted_funcs_.emplace(func);
}

void Context::removeDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
  deopted_funcs_.erase(func);
}

bool Context::addCompiledFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  return compiled_funcs_.emplace(func, compiled).second;
}

bool Context::removeCompiledFunc(BorrowedRef<PyFunctionObject> func) {
  auto in_compiled_funcs = compiled_funcs_.find(func);
  if (in_compiled_funcs != compiled_funcs_.end()) {
    in_compiled_funcs->second->removeFunction(func);
    compiled_funcs_.erase(in_compiled_funcs);
    return true;
  }
  return false;
}

bool Context::addActiveCompile(CompilationKey& key) {
  return active_compiles_.insert(key).second;
}

void Context::removeActiveCompile(CompilationKey& key) {
  active_compiles_.erase(key);
}

Ref<CompiledFunction> Context::makeCompiledFunction(
    BorrowedRef<PyFunctionObject> func,
    const CompilationKey& key,
    CompiledFunctionData&& compiled_func) {
  BorrowedRef<PyFunctionObject> outer = nullptr;
  auto outer_it = code_outer_funcs_.find(key.code);
  if (outer_it != code_outer_funcs_.end() && outer_it->second != func) {
    outer = outer_it->second;
  }
  bool immortal = getConfig().immortalize_compiled_functions ||
      (func != nullptr && _Py_IsImmortal(func)) ||
      (outer != nullptr && _Py_IsImmortal(outer));
  auto compiled = CompiledFunction::create(std::move(compiled_func), immortal);
  if (compiled == nullptr) {
    return nullptr;
  }

  // If the registered outer func for the code is different than the func we
  // will register the CompiledCode on the outer most function.
  if (outer != nullptr && outer->func_globals == key.globals &&
      outer->func_builtins == key.builtins &&
      !associateFunctionWithCompiled(outer, compiled, true)) {
    return nullptr;
  }

  if (func != nullptr && !finalizeFunc(func, compiled)) {
    return nullptr;
  }

  // We are storing a borrowed reference to the CompiledFunction. For functions,
  // finalizeFunc has put the CompiledFunction in the function's dictionary to
  // keep it alive. Code objects will be deleted when we receive a notification
  // from Python that they are being destroyed.
  auto pair = compiled_codes_.emplace(key, compiled);
  JIT_CHECK(
      pair.second,
      "CompilationKey already present {}",
      PyUnicode_AsUTF8(reinterpret_cast<PyCodeObject*>(key.code)->co_qualname));
  cacheCompiledOnCode(key, compiled);
  noteCodeCompiled(key, compiled);
  return compiled;
}

#ifndef WIN32
void AotContext::init(void* bundle_handle) {
  JIT_CHECK(
      bundle_handle_ == nullptr,
      "Trying to register AOT bundle at {} but already have one at {}",
      bundle_handle,
      bundle_handle_);
  bundle_handle_ = bundle_handle;
}

void AotContext::destroy() {
  if (bundle_handle_ == nullptr) {
    return;
  }

  // TASK(T183003853): Unmap compiled functions and empty out private data
  // structures.

  dlclose(bundle_handle_);
  bundle_handle_ = nullptr;
}

void AotContext::registerFunc(const elf::Note& note) {
  elf::CodeNoteData note_data = elf::parseCodeNote(note);
  JIT_LOG("  Function {}", note.name);
  JIT_LOG("    File: {}", note_data.file_name);
  JIT_LOG("    Line: {}", note_data.lineno);
  JIT_LOG("    Hash: {:#x}", note_data.hash);
  JIT_LOG("    Size: {}", note_data.size);
  JIT_LOG("    Normal Entry: +{:#x}", note_data.normal_entry_offset);
  JIT_LOG(
      "    Static Entry: {}",
      note_data.static_entry_offset
          ? fmt::format("+{:#x}", *note_data.static_entry_offset)
          : "");

  // This could use std::piecewise_construct for better efficiency.
  auto [it, inserted] = funcs_.emplace(note.name, FuncState{});
  JIT_CHECK(inserted, "Duplicate ELF note for function '{}'", note.name);
  it->second.note = std::move(note_data);

  // Compute the compiled function's address after dynamic linking.
  void* address = dlsym(bundle_handle_, note.name.c_str());
  JIT_CHECK(
      address != nullptr,
      "Cannot find AOT-compiled function with name '{}' despite successfully "
      "loading the AOT bundle",
      note.name);
  it->second.compiled_code = {
      reinterpret_cast<const std::byte*>(address), it->second.note.size};
  JIT_LOG("    Address: {}", address);
}

const AotContext::FuncState* AotContext::lookupFuncState(
    BorrowedRef<PyFunctionObject> func) {
  std::string name = funcFullname(func);
  auto it = funcs_.find(name);
  return it != funcs_.end() ? &it->second : nullptr;
}
#endif

Context* getContext() {
  auto state = cinderx::getModuleState();
  if (state == nullptr) {
    return nullptr;
  }
  return static_cast<Context*>(state->jit_context.get());
}

} // namespace jit
