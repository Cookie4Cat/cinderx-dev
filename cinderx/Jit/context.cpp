// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "internal/pycore_ceval.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#ifndef WIN32
#include <dlfcn.h>
#include <sys/mman.h>

#include <deque>
#include <mutex>
#endif

namespace jit {

#if PY_VERSION_HEX < 0x030C0000
// 定义于 pyjit.cpp；此处前向声明以避免 context → pyjit 头文件反向依赖。
void watchFuncDeath311(BorrowedRef<PyFunctionObject> func);
#endif

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

#if PY_VERSION_HEX < 0x030C0000
// 3.11：编译后代码不经过 _PyEval_EvalFrameDefault 入口，因此绕开了解释器
// 在该入口执行的递归深度检查（tstate->recursion_remaining），深层 JIT 递归
// 会直接耗尽 C 栈（SIGSEGV 而非 RecursionError）。编译函数的 vectorcall
// 入口经此包装补齐与解释器一致的计数与检查。prologue 级检查需要帧链接前
// 的错误出口，当前代码生成层没有该路径；生成器 resume 路径不经 vectorcall，
// 仍不在保护范围内（见 M6 预演日志）。
// 试用期计时判定（go 三件套③）：编译后的前 2K 次调用按奇偶交替走
// 解释/编译两条入口并计时，等样本均时对比（带余量）裁决——编译态
// 确实劣于解释态（如天生物化负载的属性访问税）则卸载并冻结回解释
// 器。计数阈值无法区分"慢路径多但净更快"（richards）与"净更慢"
// （go），时间是唯一干净判据。嵌套/递归调用两臂同等承受计时叠加；
// 生成器函数在安装点豁免（resume 不经 vectorcall，创建耗时不代表
// 执行）。冷路径 noinline，转正后仅付一次 probation_ctl 加载。
void probationFreeze(BorrowedRef<PyFunctionObject> func);

// 试用期 v2 排队授予。v1 令牌复盘：ctl==2 等待者每次调用 CAS 同一条
// 全局缓存线，长队列（auto=4 下近 200 code 串行试用）期间整程放血
// （richards 实测 -28%，冻结数仅 1——伤害来自排队税而非误冻）。
// v2 不变量：同一时刻至多一个 code 处于计时试用（ctl==1），排队者
// （ctl==2）静默按编译态执行、零全局触碰；裁决/超时/换届时在冷路径
// 链式授予下一个。所有路径都在 GIL 下执行，互斥锁仅为防御性。
struct ProbationQueue {
  std::mutex mutex;
  std::deque<Ref<PyCodeObject>> pending;
  Ref<PyCodeObject> active;
  uint64_t active_grant_ns{0};
  size_t enrolled{0};
};

static ProbationQueue& probationQueue() {
  static ProbationQueue q;
  return q;
}

// 诊断计数（CINDERX_PROBATION_DEBUG=1 时进程退出打印）。
struct ProbationStats {
  uint64_t enrolled_direct{0};
  uint64_t enrolled_queued{0};
  uint64_t enroll_capped{0};
  uint64_t verdict_acquit{0};
  uint64_t verdict_freeze{0};
  uint64_t timeout_acquit{0};
  uint64_t reap_dethrone{0};
  uint64_t uncompiled_abort{0};
  uint64_t promoted{0};
  uint64_t pending_compiled_calls{0};
  uint64_t wrapper_interp_falls{0};
};
static ProbationStats g_probation_stats;
void probationDebugDump();
void probationDebugDump() {
  if (getenv("CINDERX_PROBATION_DEBUG") == nullptr) {
    return;
  }
  const ProbationStats& s = g_probation_stats;
  fprintf(
      stderr,
      "[probation] direct=%llu queued=%llu capped=%llu acquit=%llu "
      "freeze=%llu timeout=%llu reap=%llu uncompiled=%llu promoted=%llu\n",
      (unsigned long long)s.enrolled_direct,
      (unsigned long long)s.enrolled_queued,
      (unsigned long long)s.enroll_capped,
      (unsigned long long)s.verdict_acquit,
      (unsigned long long)s.verdict_freeze,
      (unsigned long long)s.timeout_acquit,
      (unsigned long long)s.reap_dethrone,
      (unsigned long long)s.uncompiled_abort,
      (unsigned long long)s.promoted);
  fprintf(
      stderr,
      "[probation] pending_compiled_calls=%llu wrapper_interp_falls=%llu\n",
      (unsigned long long)s.pending_compiled_calls,
      (unsigned long long)s.wrapper_interp_falls);
}

static uint64_t probationNowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
      static_cast<uint64_t>(ts.tv_nsec);
}

// 队列容量帽：auto 模式下热 code 最先到达编译，首批之外的长尾不再
// 试用（直接按正常链安装入口），把试用时代的包装器税限定在有限窗口。
constexpr size_t kProbationEnrollCap = 64;
// 持有者墙钟死线：调用稀疏的 code 不得长期占据试用位（其间新编译
// 的 code 都在排队背包装器）。超时按通过裁决（编译态无罪推定）。
// 诊断可调（CINDERX_AUTOJIT_PROBATION_DEADLINE_MS）。
static uint64_t probationDeadlineNs() {
  static const uint64_t ns = [] {
    const char* v = getenv("CINDERX_AUTOJIT_PROBATION_DEADLINE_MS");
    return (v != nullptr && *v != '\0') ? strtoull(v, nullptr, 10) * 1000000ull
                                        : 20'000'000ull;
  }();
  return ns;
}

// 授予下一个排队者（须持锁）。
static void probationAdvanceLocked(ProbationQueue& q) {
  q.active.reset();
  while (!q.pending.empty()) {
    Ref<PyCodeObject> code = std::move(q.pending.front());
    q.pending.pop_front();
    CodeExtra* extra = codeExtraIfExists(code);
    if (extra == nullptr || extra->probation_ctl != 2) {
      continue; // 已被他因裁决或卸载
    }
    extra->probation_seq = 0;
    extra->probation_interp_ns = 0;
    extra->probation_jit_ns = 0;
    extra->probation_ctl = 1;
    q.active_grant_ns = probationNowNs();
    q.active = std::move(code);
    return;
  }
}

// 结束当前 code 的试用（裁决出炉/超时/被他因卸载）并让贤。
static void probationConclude(CodeExtra* extra) {
  extra->probation_ctl = 0;
  ProbationQueue& q = probationQueue();
  std::lock_guard<std::mutex> lock(q.mutex);
  probationAdvanceLocked(q);
}

// 僵死持有者换届（冷路径，排队者采样调用与编译报名两处驱动）：
// 持有者被授予后若很少被调用，其计时路径不触发、死线无人检查。
// 超时视为样本不足，无罪推定（ctl=0，下次调用晋升）。
static void __attribute__((noinline)) probationReapStale() {
  ProbationQueue& q = probationQueue();
  std::lock_guard<std::mutex> lock(q.mutex);
  if (q.active != nullptr &&
      probationNowNs() - q.active_grant_ns > probationDeadlineNs()) {
    if (CodeExtra* stale = codeExtraIfExists(q.active)) {
      if (stale->probation_ctl == 1) {
        stale->probation_ctl = 0;
      }
    }
    ++g_probation_stats.reap_dethrone;
    probationAdvanceLocked(q);
  }
}

// 首次调用时报名试用（包装器 ctl==3 冷路径）。报名发生在"被调用时"
// 而非"编译完成时"——首版在 finalize 报名，队列被启动期温函数
// （最先越过 auto 阈值的 import/harness 机械）占满，且首个授予者是
// 再也不被调用的死 code，链式授予永久卡死、63 个排队者的包装器态
// 经调用方特化形连环 DEOPT 放血（richards 实测 -27%，13.7k 次排队
// 调用 ≈ 每次 ~1µs）。按调用报名后：死 code 永不报名（零成本），
// 排队者必然在被调用中，裁决必然推进，试用时代有界。
static void __attribute__((noinline)) probationEnrollOnCall(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* extra) {
  ProbationQueue& q = probationQueue();
  std::lock_guard<std::mutex> lock(q.mutex);
  if (extra->probation_ctl != 3) {
    return;
  }
  if (q.enrolled >= kProbationEnrollCap) {
    ++g_probation_stats.enroll_capped;
    extra->probation_ctl = 0; // 免试转正，下次调用晋升
    return;
  }
  ++q.enrolled;
  if (q.active == nullptr && q.pending.empty()) {
    extra->probation_seq = 0;
    extra->probation_interp_ns = 0;
    extra->probation_jit_ns = 0;
    extra->probation_ctl = 1;
    q.active_grant_ns = probationNowNs();
    q.active = Ref<PyCodeObject>::create(code);
    ++g_probation_stats.enrolled_direct;
  } else {
    extra->probation_ctl = 2;
    q.pending.emplace_back(Ref<PyCodeObject>::create(code));
    ++g_probation_stats.enrolled_queued;
  }
}

// 异常率试用(exc-rate probation,ctl==4):异常 deopt 熔断的裁决层。
// 首次越限只挂计数包装器,其后 K 次调用内统计异常 deopt 增量,
// 异常/调用 ≥ 1/2 判"每调用必炸"冻结(解释器原生处理异常,零
// deopt 税);否则永久转正(ctl=0,包装器晋升裸入口)。与计时试用
// 不同:计数无交叉污染,免全局队列,各 code 独立并行。
// 窗口 256:须跨越负载相位(首版 64 恰可整窗落在混合负载的高异常
// 率相内,copy._keep_alive 在普通 deepcopy 形被相位采样误冻)。
constexpr uint32_t kExcRateProbationCalls = 256;

static PyObject* recursionGuardedVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames);

void excRateProbationArm(BorrowedRef<PyCodeObject> code, CodeExtra* extra) {
  if (extra->probation_ctl != 0) {
    return;
  }
  Context* ctx = getContext();
  if (ctx == nullptr) {
    return;
  }
  bool armed = false;
  for (auto& entry : ctx->compiledFuncs()) {
    BorrowedRef<PyFunctionObject> f = entry.first;
    if (reinterpret_cast<PyCodeObject*>(f->func_code) == code.get()) {
      setVectorcall(f, recursionGuardedVectorcall);
      armed = true;
    }
  }
  if (armed) {
    extra->probation_seq = 0;
    extra->probation_interp_ns = extra->exc_deopt_count; // 起点快照
    extra->probation_ctl = 4;
  }
}

static void __attribute__((noinline)) excRateProbationJudge(
    BorrowedRef<PyFunctionObject> func,
    CodeExtra* extra) {
  uint32_t exc_delta = extra->exc_deopt_count -
      static_cast<uint32_t>(extra->probation_interp_ns);
  extra->probation_ctl = 0;
  // 冻结阈 40%:同一 copy._keep_alive 在 reduce 形实测率 ~50%
  // (冻结净赚 -7%)、普通 deepcopy 形 ~33%(冻结实测净亏 +11%,
  // dict/list/subdict 三次调用一次 KeyError)——经验分界取两侧
  // 实测点之间。转正判决经 skey 粘滞位固化,不再武装复审(首版
  // 转正不粘滞,计数续涨反复再武装,包装态抖动实测两基准全面劣化)。
  if (exc_delta * 5 >= kExcRateProbationCalls * 2) {
    probationFreeze(func);
  } else {
    Ci_code_extra_or_skey_release(extra, CI_CODE_EXTRA_SKEY_EXC_JUDGED_BIT);
  }
}

static PyObject* __attribute__((noinline)) probationTimedCall(
    BorrowedRef<PyFunctionObject> func,
    CodeExtra* extra,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  // 块式交替采样：32 次同臂连续调用为一块（偶块=编译、奇块=解释），
  // 每臂首块弃权（icache/分支预热），其后每臂累计 K 样本后裁决。
  // 亚微秒级调用的逐次计时被时钟开销与量化噪声支配，按块聚合把
  // 噪声均摊到块内样本上。
  constexpr uint32_t kProbationBlock = 32;
  uint32_t seq = extra->probation_seq++;
  uint32_t block = seq / kProbationBlock;
  bool interp_turn = (block & 1) != 0;
  struct timespec t0;
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
  PyObject* result;
  if (interp_turn) {
    result = getInterpretedVectorcall(func)(
        func.getObj(), stack, nargsf, kwnames);
  } else {
    CompiledFunction* compiled =
        getContext() != nullptr ? getContext()->lookupFunc(func) : nullptr;
    if (compiled == nullptr) {
      // 试用期间被他因卸载（如 despec/ROI backoff）：终止试用让贤。
      ++g_probation_stats.uncompiled_abort;
      probationConclude(extra);
      return getInterpretedVectorcall(func)(
          func.getObj(), stack, nargsf, kwnames);
    }
    result = compiled->vectorcallEntry()(func.getObj(), stack, nargsf, kwnames);
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
  uint64_t t1_ns = static_cast<uint64_t>(t1.tv_sec) * 1000000000ull +
      static_cast<uint64_t>(t1.tv_nsec);
  uint64_t ns = t1_ns -
      (static_cast<uint64_t>(t0.tv_sec) * 1000000000ull +
       static_cast<uint64_t>(t0.tv_nsec));
  if (block >= 2) {
    if (interp_turn) {
      extra->probation_interp_ns += ns;
    } else {
      extra->probation_jit_ns += ns;
    }
  }
  if (t1_ns - probationQueue().active_grant_ns > probationDeadlineNs()) {
    // 超时：样本不足以裁决，无罪推定，让贤。
    ++g_probation_stats.timeout_acquit;
    probationConclude(extra);
    return result;
  }
  size_t k = getConfig().probation_calls;
  // 总样本 = 预热两块 + 每臂 K（向上取整到块边界）。
  uint32_t accum_blocks_per_arm =
      static_cast<uint32_t>((k + kProbationBlock - 1) / kProbationBlock);
  uint32_t total = (2 + 2 * accum_blocks_per_arm) * kProbationBlock;
  if (seq + 1 >= total) {
    // 先让贤再冻结（冻结会卸载并触发入口重装，不应在持有试用位时做）。
    uint64_t interp_ns = extra->probation_interp_ns;
    uint64_t jit_ns = extra->probation_jit_ns;
    probationConclude(extra);
    if (interp_ns > 0 &&
        jit_ns * 100 > interp_ns * getConfig().probation_margin_pct) {
      ++g_probation_stats.verdict_freeze;
      probationFreeze(func);
    } else {
      ++g_probation_stats.verdict_acquit;
    }
  }
  return result;
}

// 编译入口每调用查找的快路径。CodeExtra.jit_compiled 是 finalize/
// uncompile 双侧维护的 (code, globals, builtins) 精确元组缓存（见
// cacheCompiledOnCode/uncacheCompiledOnCode 与函数创建重挂接读者），
// 此处 co_extra 行内直读 + 两次指针比较即可命中绝大多数调用；任何
// 不符（无 extra/缓存空/globals 或 builtins 不同）回落 compiled_codes_
// 哈希查找。守卫包装此前每次调用都付哈希（CI_JIT_NO_ENTRY_GUARD
// 注释点名的三项每调用开销之一，PMP 各调用密集项 ~2%）。
static CompiledFunction* lookupCompiledForCall(
    BorrowedRef<PyFunctionObject> func) {
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state != nullptr) {
    // co_extra 行内直读（共享镜像结构，见 code_extra.h 论证）。
    CodeExtra* extra =
        Ci_code_extra_fast_read_311(code, mod_state->code_extra_index);
    {
      if (extra != nullptr) {
        auto* compiled = reinterpret_cast<CompiledFunction*>(
            _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
        if (compiled != nullptr && extra->jit_globals == func->func_globals &&
            extra->jit_builtins == func->func_builtins) {
          return compiled;
        }
      }
    }
  }
  return getContext() != nullptr ? getContext()->lookupFunc(func) : nullptr;
}

static PyObject* recursionGuardedVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  PyThreadState* tstate = PyThreadState_GET();
  BorrowedRef<PyFunctionObject> func{func_obj};
  // D8 tracing pause（预演版）：tracing/profiling 激活期间新调用一律进入
  // 解释器（生成码不产生 trace 事件）。已在栈上的 JIT 帧不受影响；解释器
  // 入口自带递归计数，此路径不重复计数。
  if (tstate->cframe->use_tracing) {
    return getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);
  }
  if (_Py_EnterRecursiveCallTstate(tstate, "")) {
    return nullptr;
  }
  PyObject* result;
  // 每调用热路径预算：一次全局节拍自增+掩码测试。策略工作（codeExtra
  // 查表、计数、窗口裁决）1/16 采样进入；计时试用（研究旋钮）启用时
  // 才逐调用查表。
  static const size_t kPressureRatio = getConfig().ic_pressure_ratio;
  static const bool kTimedProbation = getConfig().probation_calls > 0;
  static const bool kExcFuse = getConfig().exc_deopt_fuse;
  static uint64_t g_call_tick = 0;
  CodeExtra* extra = nullptr;
  if (kTimedProbation || kExcFuse) {
    // co_extra 行内快读(与 lookupCompiledForCall 同款):本包装器是
    // 未行内化守卫函数的默认入口,exc-fuse 默认开使此路径每调用执行
    // ——出线 _PyCode_GetExtra 版本实测 deepcopy/pprint +15%。
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state != nullptr) {
      extra = Ci_code_extra_fast_read_311(
          reinterpret_cast<PyCodeObject*>(func->func_code),
          mod_state->code_extra_index);
    }
  }
  if (kPressureRatio > 0 && ((++g_call_tick & 15) == 0)) {
    // IC 压力密度窗口裁决（生产判据）：采样计调用（×16 折算），每
    // 折算 4096 次调用对比窗内 stub 慢路径进入数，密度超阈即卸载
    // 冻结——时间上编译态劣于解释器行内特化的形态（天生物化负载）
    // 密度比均势负载高一个量级（计数矩阵实测 ~11 vs ~1.7 每调用）。
    if (extra == nullptr) {
      extra =
          codeExtraIfExists(reinterpret_cast<PyCodeObject*>(func->func_code));
    }
    if (extra != nullptr) {
      Ci_code_extra_incr_calls(extra);
      uint64_t sampled = Ci_code_extra_get_calls(extra);
      if ((sampled & 255) == 0) { // ≈4096 次调用
        if (extra->ic_slow_pressure > kPressureRatio * 4096) {
          _Py_LeaveRecursiveCallTstate(tstate);
          probationFreeze(func);
          return getInterpretedVectorcall(func)(
              func_obj, stack, nargsf, kwnames);
        }
        extra->ic_slow_pressure = 0;
      }
    }
  }
  if (kTimedProbation && extra != nullptr && extra->probation_ctl == 1) {
    result = probationTimedCall(func, extra, stack, nargsf, kwnames);
  } else if (CompiledFunction* compiled = lookupCompiledForCall(func)) {
    if (kExcFuse && extra != nullptr && extra->probation_ctl == 4) {
      // 异常率试用:按调用计数,K 次后裁决(异常增量由 deopt 路径
      // 记入 exc_deopt_count)。
      if (++extra->probation_seq >= kExcRateProbationCalls) {
        excRateProbationJudge(func, extra);
      }
    } else if (kExcFuse && !kTimedProbation && extra != nullptr &&
               extra->probation_ctl == 0 &&
               compiled->runtime()->entryGuardInlined()) {
      // 试用结束(转正)后晋升裸入口,包装器退场。
      setVectorcall(func, compiled->vectorcallEntry());
    }
    if (kTimedProbation && extra != nullptr) {
      if (extra->probation_ctl == 0) {
        // 已通过裁决且入口守卫已行内化：晋升裸编译入口，本包装器就此
        // 退场（v1 的通过者永久背包装器哈希税在此消除）。
        if (compiled->runtime()->entryGuardInlined()) {
          setVectorcall(func, compiled->vectorcallEntry());
          ++g_probation_stats.promoted;
        }
      } else if (extra->probation_ctl == 3) {
        probationEnrollOnCall(
            BorrowedRef<PyCodeObject>{func->func_code}, extra);
      } else if (++g_probation_stats.pending_compiled_calls &&
                 (++extra->probation_seq & 255) == 0) {
        // 排队者（ctl==2）以自身调用采样驱动队列活性兜底（持有者
        // 中途被卸载且不再被调用时换届）。
        probationReapStale();
      }
    }
    result = compiled->vectorcallEntry()(func_obj, stack, nargsf, kwnames);
  } else {
    // 安装与调用之间函数被去优化：退回解释器入口。
    ++g_probation_stats.wrapper_interp_falls;
    result = getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);
  }
  _Py_LeaveRecursiveCallTstate(tstate);
  return result;
}
#endif

bool isRecursionGuardVectorcall(vectorcallfunc entry) {
#if PY_VERSION_HEX < 0x030C0000
  return entry == recursionGuardedVectorcall;
#else
  (void)entry;
  return false;
#endif
}

vectorcallfunc jitVectorcallEntryBase(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  if (Context* ctx = getContext()) {
    if (CompiledFunction* compiled = ctx->lookupFunc(func)) {
      return compiled->vectorcallEntry();
    }
  }
#endif
  return func->vectorcall;
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

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 无 function watcher：布防 weakref 死亡看护，函数对象析构时
  // 走完整 funcDestroyed 注销链（缺失时注册表悬垂，finalize() 对尸体
  // 写 vectorcall——异步负载 auto>2 实测 SIGSEGV）。声明见 pyjit.h。
  watchFuncDeath311(func);
#endif

#if PY_VERSION_HEX < 0x030C0000
  // CI_JIT_NO_ENTRY_GUARD=1（M9 预演专用）：安装裸编译入口以隔离守卫
  // 包装的每调用开销（哈希查找 + 额外 C 帧 + 递归计数），代价是放弃
  // 递归深度检查与 tracing pause——仅限基准测量，不得用于正确性口径。
  static const bool no_entry_guard = [] {
    const char* v = getenv("CI_JIT_NO_ENTRY_GUARD");
    return v != nullptr && *v != '\0' && *v != '0';
  }();
  bool probation_armed = false;
  if (!no_entry_guard && getConfig().probation_calls > 0) {
    // 试用期武装（覆盖全部 code，含入口守卫行内化者）：仅置位挂
    // 包装器，报名推迟到首次真实调用（见 probationEnrollOnCall 注释
    // ——finalize 期报名会被启动期温函数占满队列且死 code 卡死链）。
    // 生成器豁免（resume 不经 vectorcall）；despec 重编译后允许再次
    // 试用。通过裁决后由包装器晋升裸入口，武装态对不被调用的 code
    // 零成本。
    BorrowedRef<PyCodeObject> code{func->func_code};
    if (!(code->co_flags & kCoFlagsAnyGenerator)) {
      if (CodeExtra* extra = codeExtra(code)) {
        if (extra->probation_ctl == 0) {
          extra->probation_ctl = 3;
        }
        probation_armed = true;
      }
    }
  }
  if (probation_armed) {
    setVectorcall(func, recursionGuardedVectorcall);
  } else if (no_entry_guard) {
    setVectorcall(func, compiled->vectorcallEntry());
  } else if (compiled->runtime()->entryGuardInlined()) {
    // 入口守卫行内化：两检查已下沉编译序言（tracing 分流 + 递归预检
    // /账本），守卫包装消解，直装编译入口。资格与发射同源
    //（CodeRuntime 旗标，LIR 生成期判定）。
    setVectorcall(func, compiled->vectorcallEntry());
  } else {
    setVectorcall(func, recursionGuardedVectorcall);
  }
#else
  setVectorcall(func, compiled->vectorcallEntry());
#endif
  if (hasFunctionEntryCache(func)) {
    void** indirect = findFunctionEntryCache(func);
    *indirect = compiled->staticEntry();
  }

  // Associate the function with the CompiledFunction for GC tracking.
  // This is ultimately what will keep the CompiledFunction alive and
  // keep the PyFunctionObject JITed.
  return associateFunctionWithCompiled(func, compiled, false /* is_nested */);
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
