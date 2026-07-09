// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/inline_cache.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <algorithm>
#include <memory>

namespace jit {

namespace {

template <class T>
struct TypeWatcher {
  jit::UnorderedMap<BorrowedRef<PyTypeObject>, jit::UnorderedSet<T*>> caches;

  void watch(BorrowedRef<PyTypeObject> type, T* cache) {
    if (PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE) &&
        !PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
      return;
    }
    JIT_CHECK(
        cinderx::getModuleState()->watcher_state.watchType(type) == 0,
        "Failed to watch type {} for attribute cache",
        type->tp_name);
    caches[type].emplace(cache);
  }

  void unwatch(BorrowedRef<PyTypeObject> type, T* cache) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    it->second.erase(cache);
    // don't unwatch type; other watchers may still be watching it
  }

  template <typename Callback>
  void typeChanged(BorrowedRef<PyTypeObject> type, Callback cb) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    jit::UnorderedSet<T*> to_notify = std::move(it->second);
    caches.erase(it);
    for (T* cache : to_notify) {
      cb(cache, type);
    }
  }

  void typeChanged(BorrowedRef<PyTypeObject> type) {
    typeChanged(type, [](T* cache, BorrowedRef<PyTypeObject> tp) {
      cache->typeChanged(tp);
    });
  }
};

TypeWatcher<AttributeCache> ac_watcher;
TypeWatcher<AttributeCache> ac_descr_watcher;
TypeWatcher<LoadTypeAttrCache> ltac_watcher;
TypeWatcher<LoadMethodCache> lm_watcher;
TypeWatcher<LoadTypeMethodCache> ltm_watcher;

// Sentinel PyTypeObject that must never escape into user code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject s_empty_type_attr_cache = {
    PyVarObject_HEAD_INIT(NULL, 0) "EmptyLoadTypeAttrCache",
};
#pragma clang diagnostic pop

inline PyDictObject* get_dict(PyObject* obj, Py_ssize_t dictoffset) {
  PyObject** dictptr = (PyObject**)((char*)obj + dictoffset);
  return (PyDictObject*)*dictptr;
}

inline PyDictObject* get_or_allocate_dict(
    PyObject* obj,
    Py_ssize_t dict_offset) {
  PyDictObject* dict = get_dict(obj, dict_offset);
  if (dict == nullptr) {
    dict =
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr));
    if (dict == nullptr) {
      return nullptr;
    }
    Py_DECREF(dict);
  }
  return dict;
}

#if PY_VERSION_HEX < 0x030C0000
// slot_tp_getattr_hook 探针（劣化归因轮 C2）：定义 __getattr__（而非
// __getattribute__）的类，tp_getattro 为 typeobject.c 的静态函数
// slot_tp_getattr_hook——语义 = 先走 GenericGetAttr，AttributeError
// 时才进 __getattr__。该符号不导出，首次调用时经探针类捕获。命中侧
// 缓存对这类接收者语义不变（通用阶段找到即 hook 的返回值），可安全
// 放行填充；探针失败返回 nullptr，一切比较落空即行为不变。
getattrofunc ciSlotTpGetattrHook() {
  // 惰性初始化必须对"在异常传播中途被首次触发"免疫：执行探针类
  // 创建前 Fetch 保存在途异常、结束后 Restore——否则本函数内的
  // PyErr_Clear 会清掉别人的在途异常，NULL 无异常上浮成
  // SystemError（PGO 构建下 import enum 途中首触发的实测事故；
  // 另有 warmSlotTpGetattrHookProbe 在 init 期预热消灭惰性窗口，
  // 本保护为纵深防御）。
  static getattrofunc hook = []() -> getattrofunc {
    PyObject *exc_type, *exc_value, *exc_tb;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    getattrofunc result = nullptr;
    {
      auto globals = Ref<>::steal(PyDict_New());
      if (globals != nullptr) {
        auto res = Ref<>::steal(PyRun_String(
            "class _CixGetattrProbe:\n"
            "    def __getattr__(self, name):\n"
            "        raise AttributeError(name)\n",
            Py_file_input,
            globals,
            globals));
        if (res != nullptr) {
          PyObject* cls = PyDict_GetItemString(globals, "_CixGetattrProbe");
          if (cls != nullptr && PyType_Check(cls)) {
            getattrofunc fn = reinterpret_cast<PyTypeObject*>(cls)->tp_getattro;
            result = fn == PyObject_GenericGetAttr ? nullptr : fn;
          }
        } else {
          PyErr_Clear();
        }
      } else {
        PyErr_Clear();
      }
    }
    PyErr_Restore(exc_type, exc_value, exc_tb);
    return result;
  }();
  return hook;
}
#endif

PyObject* __attribute__((noinline)) raise_attribute_error(
    PyObject* obj,
    PyObject* name) {
#if PY_VERSION_HEX < 0x030C0000
  // 各缓存 kind 的"确定 miss"统一漏斗。__getattr__ 类（C2 放行填充
  // 的接收者）在此不得直接抛错——hook 语义为通用阶段未找到时进
  // __getattr__ 兜底。改走完整协议（含 __getattr__；sphinx Config
  // 惰性填充模式实测教训：新实例空槽 miss 直接抛错会绕过兜底，
  // 全部构建瞬间失败）。通用类行为不变。
  getattrofunc hook = ciSlotTpGetattrHook();
  if (hook != nullptr && Py_TYPE(obj)->tp_getattro == hook) {
    return PyObject_GetAttr(obj, name);
  }
#endif
  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      Py_TYPE(obj)->tp_name,
      name);
  Cix_set_attribute_error_context(obj, name);
  return nullptr;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<PyModuleObject> mod) {
  if (mod->md_dict) {
    BorrowedRef<PyDictObject> md_dict = mod->md_dict;
    return Ci_DictVersionTag(md_dict.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<Ci_StrictModuleObject> mod) {
  if (mod->globals) {
    BorrowedRef<PyDictObject> globals = mod->globals;
    return Ci_DictVersionTag(globals.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else {
    return 0;
  }
}

void maybeCollectCacheStats(
    std::unique_ptr<CacheStats>& stat,
    BorrowedRef<PyTypeObject> tp,
    BorrowedRef<> name,
    CacheMissReason reason) {
  // stat 可为空：initCacheStats 只在 LIR 分配点按旗标配套调用，运行期
  // 增设的缓存（如 lm 的类型接收者委托实例）未必初始化过。
  if (stat == nullptr || !getConfig().collect_attr_cache_stats) {
    return;
  }
  std::string key =
      fmt::format("{}.{}", typeFullname(tp), PyUnicode_AsUTF8(name));
  stat->misses.insert({key, CacheMiss{0, reason}}).first->second.count++;
}

} // namespace

ICRuntimeStats g_ic_runtime_stats;

// la_slow 站点归属直方图（计数模式专用；键 = 接收者类型.属性名）。
std::unordered_map<std::string, uint64_t>& icSlowSiteHistogram() {
  static std::unordered_map<std::string, uint64_t> histogram;
  return histogram;
}

static void recordICSlowSite(BorrowedRef<> obj, BorrowedRef<> name) {
  if (!getConfig().collect_attr_cache_stats) {
    return;
  }
  std::string key = fmt::format(
      "{}.{}", Py_TYPE(obj.get())->tp_name, PyUnicode_AsUTF8(name));
  icSlowSiteHistogram()[key]++;
}

void AttributeMutator::changeKindFromSplitInline(
    SplitMutator* split,
    Kind new_kind) {
  AttributeMutator* mutator = reinterpret_cast<AttributeMutator*>(
      reinterpret_cast<uintptr_t>(split) - offsetof(AttributeMutator, split_));
  mutator->type_ = reinterpret_cast<uintptr_t>(mutator->type()) |
      static_cast<uintptr_t>(new_kind);
}

PyDictKeysObject* getSplitKeys(BorrowedRef<PyTypeObject> type) {
  assert(PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE));
  PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(type.get());
  return ht->ht_cached_keys;
}

bool SplitMutator::canInsertToSplitDict(
    BorrowedRef<PyDictObject> dict,
    BorrowedRef<> name) {
  if (dict->ma_keys != keys) {
    return false;
  }
  // In 3.12 we can insert in any order, we just need to update the insertion
  // order
  return (
      val_offset != -1 || (val_offset = getDictKeysIndex(keys, name)) != -1);
}

bool SplitMutator::ensureValueOffset(BorrowedRef<> name) {
  if (val_offset == -1) {
    val_offset = getDictKeysIndex(keys, name);
    if (val_offset == -1) {
      return false;
    }
  }
  return true;
}

#if PY_VERSION_HEX >= 0x030E0000
PyObject* SplitMutator::getAttrInline(PyObject* obj, PyObject* name) {
  if (!ensureValueOffset(name)) {
    return PyObject_GetAttr(obj, name);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitInlineKnownOffset);
  return getAttrInlineKnownOffset(obj, name);
}

PyObject* SplitMutator::getAttrInlineKnownOffset(
    PyObject* obj,
    PyObject* name) {
  PyDictValues* values = _PyObject_InlineValues(obj);
  if (!values->valid) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplitKnownOffset);
    return getAttr(obj, name);
  }
  PyObject* result = values->values[val_offset];
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  return Py_NewRef(result);
}

PyObject* SplitMutator::getAttrSlowPath(
    PyObject* obj,
    PyObject* name,
    BorrowedRef<PyDictObject> dict) {
  PyObject* attr_o;
  int res = [&] {
    auto strong_ref = Ref<>::create(dict);
    return PyDict_GetItemRef(dict, name, &attr_o);
  }();
  if (res == 0) {
    return raise_attribute_error(obj, name);
  }
  if (res == -1) {
    return nullptr;
  }
  return attr_o;
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);
  if (!ensureValueOffset(name)) {
    return getAttrSlowPath(obj, name, dict);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitKnownOffset);
  return getAttrKnownOffset(obj, name);
}

PyObject* SplitMutator::getAttrKnownOffset(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);
  if (dict->ma_keys != keys) {
    return getAttrSlowPath(obj, name, dict);
  }
  JIT_DCHECK(
      DK_IS_UNICODE(keys) && val_offset < keys->dk_nentries,
      "Expected dictionary keys object to change");
  PyObject* attr_o = dict->ma_values->values[val_offset];
  if (attr_o == nullptr) {
    return raise_attribute_error(obj, name);
  }
  return Py_NewRef(attr_o);
}

int SplitMutator::setAttrInline(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  if (!ensureValueOffset(name)) {
    return PyObject_SetAttr(obj, name, value);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitInlineKnownOffset);
  return setAttrInlineKnownOffset(obj, name, value);
}

int SplitMutator::setAttrInlineKnownOffset(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  PyDictValues* values = _PyObject_InlineValues(obj);
  PyDictObject* dict = _PyObject_GetManagedDict(obj);
  if (!values->valid || dict) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplitKnownOffset);
    return setAttr(obj, name, value);
  }
  auto old_value = Ref<>::steal(values->values[val_offset]);
  values->values[val_offset] = Py_NewRef(value);
  if (!old_value) {
    _PyDictValues_AddToInsertionOrder(values, val_offset);
  }
  return 0;
}

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  if (!ensureValueOffset(name)) {
    return PyObject_SetAttr(obj, name, value);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitKnownOffset);
  return setAttrKnownOffset(obj, name, value);
}

int SplitMutator::setAttrKnownOffset(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);
  if (dict == nullptr) {
    return PyObject_SetAttr(obj, name, value);
  }
  if (keys != dict->ma_keys) {
    // Slow path
    auto strong_ref = Ref<>::create(dict);
    return PyDict_SetItem(dict, name, value);
  }
  _PyDict_InsertSplitValue(dict, name, value, val_offset);
  return 0;
}

#elif PY_VERSION_HEX >= 0x030C0000

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    // Values are stored in a values array not attached to a dictionary.
    if (!ensureValueOffset(name)) {
      return PyObject_SetAttr(obj, name, value);
    }

    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* old_value = values->values[val_offset];
    values->values[val_offset] = value;
    Py_INCREF(value);
    if (old_value == nullptr) {
      _PyDictValues_AddToInsertionOrder(values, val_offset);
    } else {
      Py_DECREF(old_value);
    }
    return 0;
  }

  // Dictionary has been materialized but may still be using shared keys.
  BorrowedRef<PyDictObject> dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    dict =
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr));
    if (dict == nullptr) {
      return -1;
    }
    Py_DECREF(dict);
  }

  if (dict == nullptr) {
    return -1;
  }

  if (canInsertToSplitDict(dict, name)) {
    PyObject* old_value = DICT_VALUES(dict.get())[val_offset];
    if (old_value == nullptr) {
      // Track insertion order on 3.12.
      _PyDictValues_AddToInsertionOrder(dict->ma_values, val_offset);
    }
    if (!_PyObject_GC_IS_TRACKED(dict.getObj())) {
      if (_PyObject_GC_MAY_BE_TRACKED(value)) {
        PyObject_GC_Track(dict.getObj());
      }
    }

    uint64_t new_version =
        _PyDict_NotifyEvent(PyDict_EVENT_MODIFIED, dict, name, value);

    Py_INCREF(value);
    DICT_VALUES(dict.get())[val_offset] = value;
    dict->ma_version_tag = new_version;

    if (old_value == nullptr) {
      dict->ma_used++;
    } else {
      Py_DECREF(old_value);
    }

    return 0;
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    if (!ensureValueOffset(name)) {
      return raise_attribute_error(obj, name);
    }
    // Values are stored in values w/o materialized dictionary
    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* result = values->values[val_offset];
    if (result == nullptr) {
      return raise_attribute_error(obj, name);
    }
    Py_INCREF(result);
    return result;
  }

  PyDictObject* dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    return raise_attribute_error(obj, name);
  }
  PyObject* result = nullptr;
  if (dict->ma_keys == keys) {
    if (!ensureValueOffset(name)) {
      return raise_attribute_error(obj, name);
    }
    // We are still sharing keys with the inline object.
    result = DICT_VALUES(dict)[val_offset];
  } else {
    auto dictobj = reinterpret_cast<PyObject*>(dict);
    Py_INCREF(dictobj);
    result = PyDict_GetItem(dictobj, name);
    Py_DECREF(dictobj);
  }
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(result);
  return result;
}
#else

// 3.11 managed dict 预头双槽：-4 = PyDictValues*（非空即 values 形态），
// -3 = PyDictObject*（物化后使用）。见 CPython 3.11
// _PyObject_ValuesPointer / _PyObject_ManagedDictPointer。
static inline PyDictValues* ci_inline_values_311(PyObject* obj) {
  return *reinterpret_cast<PyDictValues**>(
      reinterpret_cast<char*>(obj) - 4 * sizeof(PyObject*));
}

static inline PyDictObject* ci_managed_dict_311(PyObject* obj) {
  return *reinterpret_cast<PyDictObject**>(
      reinterpret_cast<char*>(obj) - 3 * sizeof(PyObject*));
}

// [P2] PEP 509 版本发号：影子发号器（定义于 cinderx_ceval_shims.c，
// Ci_InitOpcodes 以运行时当前值 + 2^40 播种，与 libpython 内部计数器
// 不可能撞号）。物化实例字典是真实字典对象，覆写须镜像 stock
// STORE_ATTR_WITH_HINT 的版本戳。
extern "C" uint64_t ci_pydict_global_version_shadow;

// 带 hint 的 unicode keys 名字定位（镜像 stock *_WITH_HINT 设计）：
// me_key 指针比较自验证——hint 无论新旧乃至未初始化，越界或键不符即
// 线性重算，无需任何版本前提。返回条目下标或 -1（名字不在键中）。
static inline Py_ssize_t ci_hinted_keys_index_311(
    PyDictKeysObject* dk,
    PyObject* name,
    Py_ssize_t* hint_io) {
  Py_ssize_t hint = *hint_io;
  if (hint >= 0 && hint < dk->dk_nentries &&
      DK_UNICODE_ENTRIES(dk)[hint].me_key == name) {
    return hint;
  }
  hint = getDictKeysIndex(dk, name);
  *hint_io = hint;
  return hint;
}

// 3.11：无副作用的实例属性直读（借引用；不存在返回 nullptr）。
// _PyObject_GetDictPtr 在 3.11 对 values 形态实例有物化副作用（把
// 共享 values 转为真实字典对象），任何高频慢路径禁用之——方法慢
// 路径的物化副作用曾把整个工作负载的新生实例批量转入慢形态（IC
// 计数轮 go 案）。values 形态经共享键定位直读；物化实例经 -3 槽
// 字典查找（管理型实例字典恒为 unicode 键）。
// split 包装字典的 ma_values 是实例创建时的原 values 数组，容量
// （预头 [-1] 字节）定格；ma_keys 为其后仍可成长的共享键——hint
// 虽 < dk_nentries 仍可能 ≥ 包装数组容量，越界即回落（go 三件套②
// 排障实证：store stub 物化写越界 → 任意堆写）。
static inline bool ci_split_values_in_capacity_311(
    PyDictValues* values,
    Py_ssize_t ix) {
  return ix < reinterpret_cast<uint8_t*>(values)[-1];
}

static PyObject* ci_peek_instance_attr_hinted_311(
    PyObject* obj,
    PyObject* name,
    Py_ssize_t* hint_io) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (!PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    return nullptr;
  }
  PyDictValues* values = ci_inline_values_311(obj);
  if (values != nullptr) {
    PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
    PyDictKeysObject* dk = ht->ht_cached_keys;
    if (dk == nullptr || !DK_IS_UNICODE(dk)) {
      return nullptr;
    }
    Py_ssize_t ix = ci_hinted_keys_index_311(dk, name, hint_io);
    return ix >= 0 ? values->values[ix] : nullptr;
  }
  PyDictObject* dict = ci_managed_dict_311(obj);
  if (dict == nullptr) {
    return nullptr;
  }
  // 物化实例：unicode 键经 hint 直读（split 包装读 ma_values、
  // combined 读 me_value——me_key 自验证故 hint 在两种键对象间
  // 迁移也安全）；general 键罕见形态走通用查找。
  if (DK_IS_UNICODE(dict->ma_keys)) {
    Py_ssize_t ix = ci_hinted_keys_index_311(dict->ma_keys, name, hint_io);
    if (ix < 0) {
      return nullptr;
    }
    if (dict->ma_values != nullptr) {
      return ci_split_values_in_capacity_311(dict->ma_values, ix)
          ? dict->ma_values->values[ix]
          : nullptr;
    }
    return DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
  }
  return PyDict_GetItem(reinterpret_cast<PyObject*>(dict), name);
}

static PyObject* ci_peek_instance_attr_311(PyObject* obj, PyObject* name) {
  Py_ssize_t hint = -1;
  return ci_peek_instance_attr_hinted_311(obj, name, &hint);
}

// values 形槽位写入（覆写与插入，镜像 stock STORE_ATTR_INSTANCE_VALUE
// 两分支）。返回 0 成功；-1 表示插入超容量，调用方回落通用协议
// （stock 同点物化）。ix 须已通过容量守卫。
static inline int ci_values_slot_store_311(
    PyDictValues* values,
    Py_ssize_t ix,
    PyObject* value) {
  PyObject* old = values->values[ix];
  if (old != nullptr) {
    Py_INCREF(value);
    values->values[ix] = value;
    Py_DECREF(old);
    return 0;
  }
  uint8_t* size_ptr = reinterpret_cast<uint8_t*>(values) - 2;
  int size = *size_ptr;
  if (size + 2 < reinterpret_cast<uint8_t*>(values)[-1]) {
    size++;
    size_ptr[-size] = static_cast<uint8_t>(ix);
    *size_ptr = size;
    Py_INCREF(value);
    values->values[ix] = value;
    return 0;
  }
  return -1;
}

// 物化实例字典的槽位覆写（GC 跟踪保障 + PEP 509 版本戳，镜像 stock
// STORE_ATTR_WITH_HINT 的 old 非空路径）。返回 0 成功；-1 表示槽空
// （插入），调用方回落通用协议。
static inline int ci_mat_dict_slot_store_311(
    PyDictObject* dict,
    PyObject** slot,
    PyObject* value) {
  PyObject* old = *slot;
  if (old == nullptr) {
    return -1;
  }
  if (!_PyObject_GC_IS_TRACKED(reinterpret_cast<PyObject*>(dict)) &&
      _PyObject_GC_MAY_BE_TRACKED(value)) {
    PyObject_GC_Track(reinterpret_cast<PyObject*>(dict));
  }
  Py_INCREF(value);
  *slot = value;
  dict->ma_version_tag = ++ci_pydict_global_version_shadow;
  Py_DECREF(old);
  return 0;
}

// 3.11 写侧覆写快路径（IC 计数轮：richards/raytrace 每窗口数百万次
// STORE_ATTR 全部落在既有值槽覆写）。等价于 stock
// STORE_ATTR_INSTANCE_VALUE 的 old != NULL 分支：values 形态实例无
// 独立字典对象、无版本号可失效，直接槽位替换。新属性插入（槽位空，
// 需 _PyDictValues_AddToInsertionOrder 插入序记录）与物化实例回退
// 通用协议。有效性前提同读侧：调用方 matches() 已做 tp_version_tag
// 拉式校验，共享键容量固定故 val_offset 恒在实例 values 容量内。
int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  if (ensureValueOffset(name)) {
    PyDictValues* values = ci_inline_values_311(obj);
    if (values != nullptr) {
      PyObject* old = values->values[val_offset];
      if (old != nullptr) {
        incICStat(g_ic_runtime_stats.sa_values_overwrite);
        Py_INCREF(value);
        values->values[val_offset] = value;
        Py_DECREF(old);
        return 0;
      }
      // 插入分支：镜像 stock STORE_ATTR_INSTANCE_VALUE 的 old == NULL
      // 路径（值写入 + 插入序记录）。_PyDictValues_AddToInsertionOrder
      // 为内部符号不可链接，此处按 vendored 3.11.6 的 values 预头布局
      // 逐字复刻：[-1]=容量、[-2]=已插入数、[-2-size]=插入序字节。
      // 容量保护与 stock 的 assert 同判据，越界回退通用协议（该形态
      // 意味着实例应物化，交由 PyObject_SetAttr 处理）。
      uint8_t* size_ptr = reinterpret_cast<uint8_t*>(values) - 2;
      int size = *size_ptr;
      if (size + 2 < reinterpret_cast<uint8_t*>(values)[-1]) {
        incICStat(g_ic_runtime_stats.sa_values_insert);
        size++;
        size_ptr[-size] = static_cast<uint8_t>(val_offset);
        *size_ptr = size;
        Py_INCREF(value);
        values->values[val_offset] = value;
        return 0;
      }
      return PyObject_SetAttr(obj, name, value);
    }
    // 物化实例覆写：带 hint 直读槽位（split 包装写 ma_values、combined
    // 写 me_value），镜像 stock STORE_ATTR_WITH_HINT 的 old 非空路径：
    // GC 跟踪保障 + PEP 509 版本戳（[P2] 影子发号器）。插入（槽空/键
    // 缺，涉及 ma_used 与插入序）回退通用协议。
    PyDictObject* dict = ci_managed_dict_311(obj);
    if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
      Py_ssize_t ix = ci_hinted_keys_index_311(dict->ma_keys, name, &mat_hint);
      if (ix >= 0 &&
          (dict->ma_values == nullptr ||
           ci_split_values_in_capacity_311(dict->ma_values, ix))) {
        PyObject** slot = dict->ma_values != nullptr
            ? &dict->ma_values->values[ix]
            : &DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
        PyObject* old = *slot;
        if (old != nullptr) {
          incICStat(g_ic_runtime_stats.sa_mat_overwrite);
          if (!_PyObject_GC_IS_TRACKED(reinterpret_cast<PyObject*>(dict)) &&
              _PyObject_GC_MAY_BE_TRACKED(value)) {
            PyObject_GC_Track(reinterpret_cast<PyObject*>(dict));
          }
          Py_INCREF(value);
          *slot = value;
          dict->ma_version_tag = ++ci_pydict_global_version_shadow;
          Py_DECREF(old);
          return 0;
        }
      }
    }
  }
  incICStat(g_ic_runtime_stats.sa_generic_fallback);
  return PyObject_SetAttr(obj, name, value);
}

// 读侧 values 形态快路径（M9 性能归因轮，替换原全泛型占位）。有效性
// 前提：调用方（AttributeMutator::matches）已做 tp_version_tag 拉式
// 校验；3.11 共享键容量固定（不够用即整实例物化），val_offset 恒在
// 实例 values 容量内；值槽为 NULL 时按 stock 语义抛 AttributeError
// （fill 仅在名字解析为实例属性时选择 split 形态，类侧变化由版本
// 校验拦截）。
PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  return getAttrInline(obj, name);
}

PyObject* SplitMutator::getAttrInline(PyObject* obj, PyObject* name) {
  if (!ensureValueOffset(name)) {
    // 名字不在共享键：天生物化形态（分步初始化超出共享键容量的类，
    // 实例出生即物化，属性只存在于自身组合字典的替换键集里），
    // val_offset 永远无法解析、条目终身停留本 kind——此前每次命中
    // 落 PyObject_GetAttr 全泛型（go 每窗口 400 万次）。改带 hint
    // 物化直读。缺失语义比 KnownOffset 更强：fill 选择 split 形态
    // 的前提是 _PyType_Lookup(type, name) == nullptr（类侧查无此名，
    // 连非数据描述符都没有）且类型版本被条目 matches() 钉住，故
    // 通用协议只剩实例字典一步，键缺/槽空即 AttributeError。
    PyDictValues* values = ci_inline_values_311(obj);
    if (values != nullptr) {
      // values 形态而名字不在共享键 ⇒ 实例字典必无此名。
      return raise_attribute_error(obj, name);
    }
    PyDictObject* dict = ci_managed_dict_311(obj);
    if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
      Py_ssize_t ix = ci_hinted_keys_index_311(dict->ma_keys, name, &mat_hint);
      if (ix >= 0) {
        if (dict->ma_values != nullptr &&
            !ci_split_values_in_capacity_311(dict->ma_values, ix)) {
          return raise_attribute_error(obj, name);
        }
        PyObject* result = dict->ma_values != nullptr
            ? dict->ma_values->values[ix]
            : DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
        if (result == nullptr) {
          return raise_attribute_error(obj, name);
        }
        incICStat(g_ic_runtime_stats.la_mat_hint_hit);
        Py_INCREF(result);
        return result;
      }
      return raise_attribute_error(obj, name);
    }
    // 字典缺失或 general 键等罕见形态：维持通用协议。
    return PyObject_GetAttr(obj, name);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitInlineKnownOffset);
  return getAttrInlineKnownOffset(obj, name);
}

PyObject* SplitMutator::getAttrInlineKnownOffset(
    PyObject* obj,
    PyObject* name) {
  PyDictValues* values = ci_inline_values_311(obj);
  if (values == nullptr) {
    // 实例字典已物化：带 hint 直读（IC 计数轮：go 每窗口 59 万次此
    // 形态落全泛型）。物化时 new_dict 复用共享键与 values 数组（split
    // 包装，读 ma_values）；后续键集变更转 combined（读 me_value）。
    // 缺失语义与 values 快路径同一论证：fill 仅在类侧无遮蔽时选择
    // split 形态，类型版本由条目 matches() 钉住，故槽空/键缺即
    // AttributeError。
    PyDictObject* dict = ci_managed_dict_311(obj);
    if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
      Py_ssize_t ix = ci_hinted_keys_index_311(dict->ma_keys, name, &mat_hint);
      if (ix >= 0) {
        if (dict->ma_values != nullptr &&
            !ci_split_values_in_capacity_311(dict->ma_values, ix)) {
          // 键在成长后的共享键中、但槽位超出包装数组容量：属性必然
          // 未曾写入，缺失语义同 values 快路径论证。
          return raise_attribute_error(obj, name);
        }
        PyObject* result = dict->ma_values != nullptr
            ? dict->ma_values->values[ix]
            : DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
        if (result == nullptr) {
          return raise_attribute_error(obj, name);
        }
        incICStat(g_ic_runtime_stats.la_mat_hint_hit);
        Py_INCREF(result);
        return result;
      }
      return raise_attribute_error(obj, name);
    }
    incICStat(g_ic_runtime_stats.la_split_materialized);
    return PyObject_GetAttr(obj, name);
  }
  PyObject* result = values->values[val_offset];
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  incICStat(g_ic_runtime_stats.la_split_values_hit);
  Py_INCREF(result);
  return result;
}

#endif // PY_VERSION_HEX < 0x030E0000

int CombinedMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyDictObject> dict = get_or_allocate_dict(obj, dict_offset);
  if (dict == nullptr) {
    return -1;
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* CombinedMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = get_dict(obj, dict_offset);

  if (dict == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(dict);
  PyObject* result = PyDict_GetItem(dict, name);
  Py_DECREF(dict);
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(result);
  return result;
}

int DataDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  return Py_TYPE(descr)->tp_descr_set(descr, obj, value);
}

PyObject* DataDescrMutator::getAttr(PyObject* obj) {
  return Py_TYPE(descr)->tp_descr_get(descr, obj, (PyObject*)Py_TYPE(obj));
}

int MemberDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  return PyMember_SetOne((char*)obj, memberdef, value);
}

PyObject* MemberDescrMutator::getAttr(PyObject* obj) {
  return PyMember_GetOne((char*)obj, memberdef);
}

int DescrOrClassVarMutator::setAttr(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  descrsetfunc setter = Py_TYPE(descr)->tp_descr_set;
  if (setter != nullptr) {
    auto descr_guard = Ref<>::create(descr);
    return setter(descr, obj, value);
  }
#if PY_VERSION_HEX < 0x030C0000
  // 非数据描述符的实例字典遮蔽写，按 stock GenericSetAttr 次序：
  // values 形先走 StoreInstanceAttribute 等价路径，保持 split 形态。
  // 原实现先调 _PyObject_GetDictPtr——其对 values 形实例有物化副作用
  // （stock 同位点不物化），首次遮蔽写即把接收者打成物化形，损伤
  // 后续读写快路径形态。hint 与读侧共用（values 形对共享键、物化形
  // 对字典键，me_key 自验证两态迁移安全）。
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictValues* values = ci_inline_values_311(obj);
    if (values != nullptr) {
      PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
      PyDictKeysObject* dk = ht->ht_cached_keys;
      if (dk != nullptr && DK_IS_UNICODE(dk)) {
        Py_ssize_t ix = ci_hinted_keys_index_311(dk, name, &mat_hint);
        if (ix >= 0 && ci_split_values_in_capacity_311(values, ix) &&
            ci_values_slot_store_311(values, ix, value) == 0) {
          return 0;
        }
      }
      // 名字不在共享键/超容量：回落通用协议（stock 同点物化）。
    } else {
      PyDictObject* dict = ci_managed_dict_311(obj);
      if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
        Py_ssize_t ix =
            ci_hinted_keys_index_311(dict->ma_keys, name, &mat_hint);
        if (ix >= 0 &&
            (dict->ma_values == nullptr ||
             ci_split_values_in_capacity_311(dict->ma_values, ix))) {
          PyObject** slot = dict->ma_values != nullptr
              ? &dict->ma_values->values[ix]
              : &DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
          if (ci_mat_dict_slot_store_311(dict, slot, value) == 0) {
            return 0;
          }
        }
      }
      // 插入/hint 未决：回落通用协议。
    }
  }
#endif
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == nullptr) {
    PyErr_Format(
        PyExc_AttributeError,
        "'%.50s' object attribute '%U' is read-only",
        Py_TYPE(obj)->tp_name,
        name);
    return -1;
  }
  BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
  int st = Cix_PyObjectDict_SetItem(type, obj, dictptr, name, value);
  if (st < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
    PyErr_SetObject(PyExc_AttributeError, name);
  }
  return st;
}

PyObject* DescrOrClassVarMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
  descrsetfunc setter = descr_type->tp_descr_set;
  descrgetfunc getter = descr_type->tp_descr_get;

  auto descr_guard = Ref<>::create(descr);
  if (setter != nullptr && getter != nullptr) {
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

#if PY_VERSION_HEX < 0x030C0000
  // Check instance dict（无副作用直读：_PyObject_GetDictPtr 会物化
  // values 形态实例；带 hint 免除每次访问的字符串哈希查找）。
  if (PyType_HasFeature(Py_TYPE(obj), Py_TPFLAGS_MANAGED_DICT)) {
    if (PyObject* iattr =
            ci_peek_instance_attr_hinted_311(obj, name, &mat_hint)) {
      Py_INCREF(iattr);
      return iattr;
    }
  } else {
    Ref<> dict;
    PyObject** dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr != nullptr) {
      dict.reset(*dictptr);
    }
    if (dict != nullptr) {
      if (keys_version == 0 ||
          reinterpret_cast<PyDictObject*>(dict.get())->ma_keys->dk_version !=
              keys_version) {
        auto res = Ref<>::create(PyDict_GetItem(dict, name));
        if (res != nullptr) {
          return res.release();
        }
      }
    }
  }
#else
  Ref<> dict;
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr) {
    dict.reset(*dictptr);
  }

  // Check instance dict.
  if (dict != nullptr) {
    if (keys_version == 0 ||
        reinterpret_cast<PyDictObject*>(dict.get())->ma_keys->dk_version !=
            keys_version) {
      auto res = Ref<>::create(PyDict_GetItem(dict, name));
      if (res != nullptr) {
        return res.release();
      }
    }
  }
#endif

  if (getter != nullptr) {
    // Non-data descriptor
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

  // Class var
  return descr_guard.release();
}

AttributeMutator::AttributeMutator() {
  reset();
}

void AttributeMutator::reset() {
  type_ = 0;
}

void AttributeMutator::set_combined(PyTypeObject* type) {
  set_type(type, Kind::kCombined);
  combined_.dict_offset = type->tp_dictoffset;
}

void AttributeMutator::set_data_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kDataDescr);
  data_descr_.descr = descr;
  data_descr_.descr_type = Py_TYPE(descr);
}

void AttributeMutator::set_member_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kMemberDescr);
  member_descr_.memberdef = ((PyMemberDescrObject*)descr)->d_member;
}

void AttributeMutator::set_descr_or_classvar(
    PyTypeObject* type,
    PyObject* descr,
    uint32_t keys_version) {
  set_type(type, Kind::kDescrOrClassVar);
  descr_or_cvar_.descr = descr;
  descr_or_cvar_.keys_version = keys_version;
#if PY_VERSION_HEX < 0x030C0000
  descr_or_cvar_.mat_hint = -1;
#endif
}

void AttributeMutator::set_split(
    PyTypeObject* type,
    Py_ssize_t val_offset,
    [[maybe_unused]] PyDictKeysObject* keys,
    bool inline_values) {
  set_type(type, inline_values ? Kind::kSplitInline : Kind::kSplit);
  split_.val_offset = val_offset;
  split_.keys = keys;
#if PY_VERSION_HEX < 0x030C0000
  split_.mat_hint = -1;
#endif
}

BorrowedRef<PyTypeObject> AttributeMutator::watchedDescrType() const {
  if (get_kind() == Kind::kDataDescr) {
    return data_descr_.descr_type;
  }
  return nullptr;
}

inline int
AttributeMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator setting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.setAttr(obj, name, value);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitKnownOffset:
      return split_.setAttrKnownOffset(obj, name, value);
    case AttributeMutator::Kind::kSplitInline:
      return split_.setAttrInline(obj, name, value);
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.setAttrInlineKnownOffset(obj, name, value);
#elif PY_VERSION_HEX < 0x030C0000
    // 3.11：inline 两 kind 的写侧走通用协议（读侧快路径见 SplitMutator）。
    case AttributeMutator::Kind::kSplitInline:
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.setAttr(obj, name, value);
#endif
    case AttributeMutator::Kind::kCombined:
      return combined_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kDataDescr:
      return data_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kMemberDescr:
      return member_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kDescrOrClassVar:
      return descr_or_cvar_.setAttr(obj, name, value);
    default:
      JIT_ABORT(
          "Cannot invoke setAttr for attr of kind {}", static_cast<int>(kind));
  }
}

inline PyObject* AttributeMutator::getAttr(PyObject* obj, PyObject* name) {
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator getting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.getAttr(obj, name);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitKnownOffset:
      return split_.getAttrKnownOffset(obj, name);
    case AttributeMutator::Kind::kSplitInline:
      return split_.getAttrInline(obj, name);
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.getAttrInlineKnownOffset(obj, name);
#elif PY_VERSION_HEX < 0x030C0000
    case AttributeMutator::Kind::kSplitInline:
      return split_.getAttrInline(obj, name);
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.getAttrInlineKnownOffset(obj, name);
#endif
    case AttributeMutator::Kind::kCombined:
      return combined_.getAttr(obj, name);
    case AttributeMutator::Kind::kDataDescr:
      return data_descr_.getAttr(obj);
    case AttributeMutator::Kind::kMemberDescr:
      return member_descr_.getAttr(obj);
    case AttributeMutator::Kind::kDescrOrClassVar:
      return descr_or_cvar_.getAttr(obj, name);
    default:
      JIT_ABORT(
          "Cannot invoke getAttr for attr of kind {}", static_cast<int>(kind));
  }
}

void AttributeMutator::set_type(PyTypeObject* type, Kind kind) {
  auto raw = reinterpret_cast<uintptr_t>(type);
  JIT_CHECK((raw & kindMask()) == 0, "PyTypeObject* expected to be aligned");
  auto mask = static_cast<uintptr_t>(kind);
  type_ = raw | mask;
#if PY_VERSION_HEX < 0x030C0000
  // fill 侧已由 Ci_Type_HasValidVersionTag 保证此时 tag 有效。
  type_version_ = type->tp_version_tag;
#endif
}

AttributeCache::AttributeCache() {
  for (auto& entry : entries()) {
    entry.reset();
  }
}

AttributeCache::~AttributeCache() {
  for (auto& entry : entries()) {
    if (entry.type() != nullptr) {
      ac_watcher.unwatch(entry.type(), this);
      BorrowedRef<PyTypeObject> descr_tp = entry.watchedDescrType();
      if (descr_tp != nullptr) {
        ac_descr_watcher.unwatch(descr_tp, this);
      }
      entry.reset();
    }
  }
}

void AttributeCache::typeChanged(PyTypeObject* tp) {
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
      BorrowedRef<PyTypeObject> descr_tp = entry.watchedDescrType();
      entry.reset();
      if (descr_tp != nullptr) {
        bool found = false;
        for (auto& other : entries()) {
          if (other.watchedDescrType() == descr_tp) {
            found = true;
            break;
          }
        }
        if (!found) {
          ac_descr_watcher.unwatch(descr_tp, this);
        }
      }
    }
  }
}

void AttributeCache::descrTypeChanged(PyTypeObject* tp) {
  bool found = false;
  for (auto& entry : entries()) {
    if (entry.watchedDescrType() == tp) {
      ac_watcher.unwatch(entry.type(), this);
      entry.reset();
      if (!found) {
        ac_descr_watcher.unwatch(tp, this);
        found = true;
      }
    }
  }
}

AttributeMutator* AttributeCache::findEmptyEntry() {
  auto it = std::ranges::find_if(
      entries(), [](const AttributeMutator& e) { return e.isEmpty(); });
  return it == entries().end() ? nullptr : &*it;
}

void AttributeCache::fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name) {
  BorrowedRef<> descr = _PyType_Lookup(type, name);
  fill(type, name, descr);
}

bool canCacheType(PyTypeObject* type) {
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    // We can cache values for types which have managed dictionaries on 3.12 or
    // later.
    return true;
  }

  // We only support the common case for objects - fixed-size instances
  // (tp_dictoffset >= 0) of heap types (Py_TPFLAGS_HEAPTYPE).
  return type->tp_dictoffset >= 0 &&
      PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE);
}

bool canCacheAttribute(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    uint32_t& keys_version) {
  if (type->tp_dictoffset == 0) {
    return true;
  }

  if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
    return false;
  }

  PyDictKeysObject* keys = getSplitKeys(type);
  if (keys == nullptr) {
    return false;
  }

  // If we don't have a valid keys version or the key exists in the shared
  // keys then we can't cache the value as we need to check and see if it's
  // been overridden.
  if (dictGetKeysVersion(PyInterpreterState_Get(), keys) == 0 ||
      getDictKeysIndex(keys, name) != -1) {
    return false;
  }
  keys_version = keys->dk_version;
  return true;
}

void AttributeCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    BorrowedRef<> descr) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  AttributeMutator* mut = findEmptyEntry();
  if (mut == nullptr) {
    return;
  }

  if (descr != nullptr) {
    BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
    if (descr_type->tp_descr_get != nullptr &&
        descr_type->tp_descr_set != nullptr) {
      // Data descriptor
      if (descr_type == &PyMemberDescr_Type) {
        mut->set_member_descr(type, descr);
      } else {
        if (!ensureVersionTag(descr_type)) {
          return;
        }
        // If someone modifies descr_type (e.g., deletes __set__), it may no
        // longer be a data descriptor, and the cache kind has to change.
        ac_descr_watcher.watch(descr_type, this);
        mut->set_data_descr(type, descr);
      }
    } else {
      // Non-data descriptor or class var
      uint32_t keys_version = 0;
      canCacheAttribute(type, name, keys_version);
      mut->set_descr_or_classvar(type, descr, keys_version);
    }
    ac_watcher.watch(type, this);
    return;
  }

  if (!canCacheType(type)) {
    return;
  }

  // Instance attribute with no shadowing. Specialize the lookup based on
  // whether or not the type is using split dictionaries.
  PyDictKeysObject* keys = getSplitKeys(type);
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    JIT_DCHECK(keys != nullptr, "Managed dict should have a split dict");
    bool inline_values = false;
#if PY_VERSION_HEX >= 0x030E0000
    inline_values = type->tp_flags & Py_TPFLAGS_INLINE_VALUES;
#elif PY_VERSION_HEX < 0x030C0000
    // 3.11：managed dict 类型的实例默认即 values 形态（预头 -4 槽），
    // 选择 inline 族 kind 以启用读侧快路径；个别已物化实例在
    // getAttrInlineKnownOffset 内按槽位空值回退通用协议。
    inline_values = true;
#endif
    mut->set_split(type, getDictKeysIndex(keys, name), keys, inline_values);
  } else {
    mut->set_combined(type);
  }
  ac_watcher.watch(type, this);
}

int StoreAttrCache::invoke(
    StoreAttrCache* cache,
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  return cache->doInvoke(obj, name, value);
}

int StoreAttrCache::doInvoke(PyObject* obj, PyObject* name, PyObject* value) {
  incICStat(g_ic_runtime_stats.sa_invoke);
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() != tp) {
      continue;
    }
    if (entry.matches(tp)) {
      incICStat(g_ic_runtime_stats.sa_entry_hit);
      incICStat(g_ic_runtime_stats.sa_hit_kind[entry.kindBits()]);
      return entry.setAttr(obj, name, value);
    }
    // 类型指针相同但版本失效：该条目不可能再次命中（版本号单调递增），
    // 立即清空释放槽位；期间不触碰条目内的借引用（D9）。
    entry.reset();
    break;
  }
  return invokeSlowPath(obj, name, value);
}

int __attribute__((noinline))
StoreAttrCache::invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value) {
  incICStat(g_ic_runtime_stats.sa_slow);
  int result = PyObject_SetAttr(obj, name, value);
  if (result < 0) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_SetAttr failed so there should be a Python error");
    return result;
  }

  BorrowedRef<PyTypeObject> type{Py_TYPE(obj)};
  if (type->tp_setattro == PyObject_GenericSetAttr) {
    fill(type, name);
  }

  return result;
}

PyObject*
LoadAttrCache::invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name) {
  return cache->doInvoke(obj, name);
}

PyObject* LoadAttrCache::doInvoke(PyObject* obj, PyObject* name) {
  incICStat(g_ic_runtime_stats.la_invoke);
  PyTypeObject* tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() != tp) {
      continue;
    }
    if (entry.matches(tp)) {
      incICStat(g_ic_runtime_stats.la_entry_hit);
      incICStat(g_ic_runtime_stats.la_hit_kind[entry.kindBits()]);
      return entry.getAttr(obj, name);
    }
    // 类型指针相同但版本失效：该条目不可能再次命中（版本号单调递增），
    // 立即清空释放槽位；期间不触碰条目内的借引用（D9）。
    entry.reset();
    break;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (PyType_Check(obj)) {
    if (PyObject* result = typeRecvGetAttr(obj)) {
      return result;
    }
  } else if (PyObject* result = siteExtGetAttr(obj)) {
    return result;
  }
#endif
  return invokeSlowPath(obj, name);
}

PyObject* __attribute__((noinline)) LoadAttrCache::invokeSlowPath(
    PyObject* obj,
    PyObject* name) {
  incICStat(g_ic_runtime_stats.la_slow);
  recordICSlowSite(obj, name);
  auto result = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (result == nullptr) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_GetAttr failed so there should be a Python error");
    return nullptr;
  }

  BorrowedRef<PyTypeObject> type{Py_TYPE(obj)};
  if (type->tp_getattro == PyObject_GenericGetAttr) {
    fill(type, name);
  }
#if PY_VERSION_HEX < 0x030C0000
  else if (PyType_Check(obj)) {
    // C1：类型接收者多条目缓存（元类型 getattro，通用 fill 拒收）。
    typeRecvTryFill(obj, name, result);
  } else if (
      ciSlotTpGetattrHook() != nullptr &&
      type->tp_getattro == ciSlotTpGetattrHook()) {
    // C2：__getattr__ 类（slot_tp_getattr_hook）——命中侧语义与
    // GenericGetAttr 同一。仅当结果溯源到通用阶段（实例属性直读或
    // MRO 解析同一对象）才填充；__getattr__ 兜底产物动态，不缓存。
    // 填充后命中即通用阶段命中，__getattr__ 不再参与；属性删除/类
    // 变更经既有共享键版本与 tp_version_tag 校验自然失效回慢路径，
    // 彼时重新走完整 hook 语义。
    if (ci_peek_instance_attr_311(obj, name) == result.get() ||
        _PyType_Lookup(type, name) == result.get()) {
      fill(type, name);
    }
  } else {
    siteExtTryFill(obj, name, result);
  }
#endif

  return result.release();
}

LoadTypeAttrCache::LoadTypeAttrCache() {
  reset();
}

LoadTypeAttrCache::~LoadTypeAttrCache() {
  ltac_watcher.unwatch(type_, this);
}

PyObject* LoadTypeAttrCache::invoke(
    LoadTypeAttrCache* cache,
    PyObject* obj,
    PyObject* name) {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 不发射内联 [type, value] 快路径（无 type watcher，槽对无法拉式
  // 验证），命中判定在此完成：类型指针与 tp_version_tag 双重校验（D5），
  // 校验通过前不使用缓存值（D9）。
  if (reinterpret_cast<PyObject*>(cache->type_) == obj &&
      cache->value_ != nullptr && Ci_Type_HasValidVersionTag(cache->type_) &&
      cache->type_->tp_version_tag == cache->version_) {
    Py_INCREF(cache->value_);
    return cache->value_;
  }
#endif
  // The fast path is handled by direct memory access via valueAddr().
  return cache->invokeSlowPath(obj, name);
}

PyTypeObject** LoadTypeAttrCache::typeAddr() {
  return &type_;
}

PyObject** LoadTypeAttrCache::valueAddr() {
  return &value_;
}

// NB: This function needs to be kept in sync with PyType_Type.tp_getattro.
PyObject* LoadTypeAttrCache::invokeSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> metatype{Py_TYPE(obj)};
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    return PyObject_GetAttr(obj, name);
  }

  BorrowedRef<PyTypeObject> type{obj};
  if (PyType_Ready(type) < 0) {
    return nullptr;
  }

  descrgetfunc meta_get = nullptr;
  auto meta_attribute = Ref<>::create(_PyType_Lookup(metatype, name));
  if (meta_attribute != nullptr) {
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;
    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      // Data descriptors implement tp_descr_set to intercept writes. Assume the
      // attribute is not overridden in type's tp_dict (and bases): call the
      // descriptor now.
      return meta_get(meta_attribute, type, metatype);
    }
  }

  // No data descriptor found on metatype. Look in tp_dict of this type and its
  // bases.
  auto attribute = Ref<>::create(_PyType_Lookup(type, name));
  if (attribute != nullptr) {
    // Implement descriptor functionality, if any.
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;

    meta_attribute.reset();

    bool is_cachable = local_get == nullptr;
    if (PyFunction_Check(attribute)) {
      // Loading a function from a type returns the type
      is_cachable = true;
    } else if (Py_TYPE(attribute) == &PyStaticMethod_Type) {
      // static method returns the underlying object
      attribute = Ref<>::create(Ci_PyStaticMethod_GetFunc(attribute));
      is_cachable = true;
    }
    if (!is_cachable) {
      // nullptr 2nd argument indicates the descriptor was found on the target
      // object itself (or a base).
      return local_get(attribute, nullptr, type);
    }

    fill(type, attribute);
    return attribute.release();
  }

  // No attribute found in local __dict__ (or bases): use the descriptor from
  // the metatype, if any.
  if (meta_get != nullptr) {
    return meta_get(meta_attribute, type, metatype);
  }

  // If an ordinary attribute was found on the metatype, return it now.
  if (meta_attribute != nullptr) {
    return meta_attribute.release();
  }

  // Give up.
  raise_attribute_error(obj, name);
  return nullptr;
}

void LoadTypeAttrCache::typeChanged(
    [[maybe_unused]] BorrowedRef<PyTypeObject> arg) {
  JIT_DCHECK(arg == type_, "Type watcher notified the wrong LoadTypeAttrCache");
  reset();
}

void LoadTypeAttrCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  ltac_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
#if PY_VERSION_HEX < 0x030C0000
  // 上方 Ci_Type_HasValidVersionTag 已保证此时 tag 有效。
  version_ = type->tp_version_tag;
#endif
  ltac_watcher.watch(type_, this);
}

void LoadTypeAttrCache::reset() {
  // We need to return a PyTypeObject* even in the empty case so that subsequent
  // refcounting operations work correctly.
  type_ = &s_empty_type_attr_cache;
  value_ = nullptr;
}

std::string_view kCacheMissReasons[] = {
#define NAME_REASON(reason) #reason,
    FOREACH_CACHE_MISS_REASON(NAME_REASON)
#undef NAME_REASON
};

std::string_view cacheMissReason(CacheMissReason reason) {
  return kCacheMissReasons[static_cast<size_t>(reason)];
}

LoadMethodCache::~LoadMethodCache() {
  for (auto& entry : entries_) {
    if (entry.type != nullptr) {
      lm_watcher.unwatch(entry.type, this);
      entry.type.reset();
      entry.value.reset();
    }
  }
}

LoadMethodResult LoadMethodCache::lookupHelper(
    LoadMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

// Checks to see if the cached keys version allows a lookup w/o looking in
// the dictionary. This could be either that we have a match of the keys version
// or that we have a non-heap type w/ no dictionary.
//
// Avoid using _PyObject_GetDictPtr here as it can materialize the dictionary
// on 3.12+. Instead use version-specific APIs to check the dict state without
// side effects.
bool isValidKeysVersion(uint32_t keys_version, BorrowedRef<> obj) {
  if (keys_version == 0) {
    // 0 is an invalid keys version and a sentinel value that we'll never
    // generate a cache for a heap type with. We may have a non-heap type
    // that is cached w/ a keys_version of 0 that has no dictionary in which
    // case the cache is always valid.
    return true;
  }

#if PY_VERSION_HEX >= 0x030E0000
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    if (PyType_HasFeature(tp, Py_TPFLAGS_INLINE_VALUES)) {
      PyDictValues* values = _PyObject_InlineValues(obj);
      if (values->valid) {
        // Inline values are still active but the shared keys may have changed
        // (e.g., a new instance attribute was added). Check the type's shared
        // keys version.
        PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
        return ht->ht_cached_keys->dk_version == keys_version;
      }
    }
    // Check the managed dict directly.
    PyDictObject* dict = _PyObject_GetManagedDict(obj);
    if (dict == nullptr) {
      return true;
    }
    return dict->ma_keys->dk_version == keys_version;
  }
#elif PY_VERSION_HEX >= 0x030C0000
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
    if (_PyDictOrValues_IsValues(dorv)) {
      // Values are still inline but the shared keys may have changed
      // (e.g., a new instance attribute was added). Check the type's shared
      // keys version.
      PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
      return ht->ht_cached_keys->dk_version == keys_version;
    }
    PyDictObject* dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
    if (dict == nullptr) {
      return true;
    }
    return dict->ma_keys->dk_version == keys_version;
  }
#else
  // 3.11：values 形态实例必须校验共享键版本，否则"名字已在共享键、
  // 槽位后填"的实例遮蔽形态会漏检（M9 IC 内联轮修复；此前 values
  // 形态直接落到 GetDictPtr 得 NULL 判有效）。物化实例经 -3 槽字典
  // 校验。
  {
    PyTypeObject* tp = Py_TYPE(obj.get());
    if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
      PyDictValues* values = *reinterpret_cast<PyDictValues**>(
          reinterpret_cast<char*>(obj.get()) - 4 * sizeof(PyObject*));
      if (values != nullptr) {
        PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
        return ht->ht_cached_keys != nullptr &&
            ht->ht_cached_keys->dk_version == keys_version;
      }
      PyDictObject* dict = *reinterpret_cast<PyDictObject**>(
          reinterpret_cast<char*>(obj.get()) - 3 * sizeof(PyObject*));
      if (dict == nullptr) {
        return true;
      }
      return dict->ma_keys->dk_version == keys_version;
    }
  }
#endif

  // Non-managed-dict fallback (non-heap types with tp_dictoffset).
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  assert(dictptr != nullptr);

  PyDictObject* dict = reinterpret_cast<PyDictObject*>(*dictptr);
  if (dict == nullptr) {
    return true;
  }

  return dict->ma_keys->dk_version == keys_version;
}

LoadMethodResult LoadMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  incICStat(g_ic_runtime_stats.lm_helper);
#if PY_VERSION_HEX < 0x030C0000
  // C1：类型对象作接收者——通用条目按元类型键控永不命中，慢路径又对
  // type_getattro 早退拒填。委托类型方法缓存（type_getattro 语义 +
  // tp_version_tag 拉式校验）。惰性分配。
  if (PyType_Check(obj)) {
    if (type_recv_cache_ == nullptr) {
      type_recv_cache_ = std::make_unique<LoadTypeMethodCache>();
      if (cache_stats_ != nullptr) {
        type_recv_cache_->initCacheStats(
            cache_stats_->filename.c_str(),
            cache_stats_->method_name.c_str());
      }
    }
    return type_recv_cache_->lookup(
        BorrowedRef<PyTypeObject>{reinterpret_cast<PyTypeObject*>(obj.get())},
        name);
  }
#endif
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);

  for (auto& entry : entries_) {
    if (entry.type == tp) {
#if PY_VERSION_HEX < 0x030C0000
      // D5：3.11 无 type watcher，命中前以 tp_version_tag 拉式验证；
      // 失效条目立即清空（不触碰其中借引用，D9）。
      if (!Ci_Type_HasValidVersionTag(tp) ||
          tp->tp_version_tag != entry.type_version) {
        incICStat(g_ic_runtime_stats.lm_version_fail);
        entry.type.reset();
        entry.value.reset();
        continue;
      }
#endif
      if (!isValidKeysVersion(entry.keys_version, obj)) {
        incICStat(g_ic_runtime_stats.lm_keys_fail);
#if PY_VERSION_HEX < 0x030C0000
        // 组合字典接收者（键对象已更换，如天生物化的分阶段初始化类）
        // 的共享键版本判据永不可比——改做带 hint 的实例遮蔽直判：
        // 名字不在实例字典（或 split 包装槽为空）即缓存有效（类侧
        // 变化由条目级 tp_version_tag 拉式校验钉住）。此前该形态使
        // 方法缓存楔死：规范版本未动故不驱逐、槽全占故 fill 永不
        // 成功，每次查找付 4 次键版本败 + 全量 _PyType_Lookup
        //（go 案：150 万次/窗口）。
        {
          PyDictObject* dict = ci_managed_dict_311(obj.get());
          if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
            Py_ssize_t ix =
                ci_hinted_keys_index_311(dict->ma_keys, name, &ia_hint_);
            bool shadowed = false;
            if (ix >= 0) {
              if (dict->ma_values != nullptr) {
                shadowed =
                    ci_split_values_in_capacity_311(dict->ma_values, ix) &&
                    dict->ma_values->values[ix] != nullptr;
              } else {
                shadowed =
                    DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value != nullptr;
              }
            }
            if (!shadowed) {
              incICStat(g_ic_runtime_stats.lm_scan_hit);
              PyObject* result = entry.value;
              Py_INCREF(result);
              Py_INCREF(obj);
              return {result, obj};
            }
          }
        }
#endif
        // 类型权威键版本已前移（共享键在 fill 后又插入了新名字，如
        // 实例属性跨方法分批添加的初始化模式）时，条目永不可再命中
        // ——不驱逐则该类型方法查找永久落慢路径且 fill 无空槽可填。
        // 物化实例的瞬时不匹配（权威版本未动）不驱逐，保住 values
        // 形态接收者的命中。
        if (PyType_HasFeature(tp, Py_TPFLAGS_HEAPTYPE)) {
          PyDictKeysObject* canonical = getSplitKeys(tp);
          if (canonical == nullptr ||
              canonical->dk_version != entry.keys_version) {
            entry.type.reset();
            entry.value.reset();
          }
        }
        continue;
      }

      incICStat(g_ic_runtime_stats.lm_scan_hit);
      PyObject* result = entry.value;
      Py_INCREF(result);
      Py_INCREF(obj);
      return {result, obj};
    }
  }

#if PY_VERSION_HEX < 0x030C0000
  // 实例属性方法位：类型指针 + VALID 标志 + tp_version_tag 拉式验证
  // （版本钉住"类侧无数据描述符遮蔽"这一 fill 前提），值经 me_key 自
  // 验证 hint 从实例 values/字典逐次活读——借引用不驻留，属性删除或
  // 实例无该键即自然未命中落慢路径。返回形态镜像 lookupSlowPath 的
  // 实例字典分支：{Py_None, attr} 双新增引用。
  if (ia_type_ == tp && Ci_Type_HasValidVersionTag(tp) &&
      tp->tp_version_tag == ia_type_version_ &&
      PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyObject* v = nullptr;
    PyObject* raw = obj.get();
    PyDictValues* values = *reinterpret_cast<PyDictValues**>(
        reinterpret_cast<char*>(raw) - 4 * sizeof(PyObject*));
    if (values != nullptr) {
      PyHeapTypeObject* ht =
          reinterpret_cast<PyHeapTypeObject*>(tp.get());
      PyDictKeysObject* dk = ht->ht_cached_keys;
      if (dk != nullptr && DK_IS_UNICODE(dk)) {
        Py_ssize_t ix = ci_hinted_keys_index_311(dk, name, &ia_hint_);
        if (ix >= 0) {
          v = values->values[ix];
        }
      }
    } else {
      PyDictObject* dict = *reinterpret_cast<PyDictObject**>(
          reinterpret_cast<char*>(raw) - 3 * sizeof(PyObject*));
      if (dict != nullptr && DK_IS_UNICODE(dict->ma_keys)) {
        Py_ssize_t ix =
            ci_hinted_keys_index_311(dict->ma_keys, name, &ia_hint_);
        if (ix >= 0) {
          if (dict->ma_values != nullptr) {
            v = ci_split_values_in_capacity_311(dict->ma_values, ix)
                ? dict->ma_values->values[ix]
                : nullptr;
          } else {
            v = DK_UNICODE_ENTRIES(dict->ma_keys)[ix].me_value;
          }
        }
      }
    }
    if (v != nullptr) {
      incICStat(g_ic_runtime_stats.lm_ia_hit);
      Py_INCREF(Py_None);
      Py_INCREF(v);
      return {Py_None, v};
    }
  }
#endif

  incICStat(g_ic_runtime_stats.lm_slow);
  return lookupSlowPath(obj, name);
}

void LoadMethodCache::typeChanged(PyTypeObject* type) {
  for (auto& entry : entries_) {
    if (entry.type == type) {
      entry.type.reset();
      entry.value.reset();
    }
  }
}

void LoadMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadMethodCache::cacheStats() {
  return cache_stats_.get();
}

LoadMethodResult __attribute__((noinline)) LoadMethodCache::lookupSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  PyTypeObject* tp = Py_TYPE(obj);
  PyObject* descr;
  descrgetfunc f = nullptr;
  PyObject **dictptr, *dict;
  PyObject* attr;
  bool is_method = false;

  if ((tp->tp_getattro != PyObject_GenericGetAttr)) {
    PyObject* res = PyObject_GetAttr(obj, name);
    if (res != nullptr) {
#if PY_VERSION_HEX < 0x030C0000
      // C2（劣化归因轮）：slot_tp_getattr_hook（定义 __getattr__ 的
      // 类）语义 = 先 GenericGetAttr、未找到才进 __getattr__。命中
      // 溯源到类侧函数（绑定方法的 __func__ 与 MRO 解析同一）即回填
      // 通用条目——后续命中走条目/桩快路径，实例遮蔽由共享键版本
      // 防护；类侧变更由 tp_version_tag 拉式校验。填充后通用阶段
      // 必命中，__getattr__ 不再参与，语义不变。__getattr__ 兜底
      // 产物（代理转发等动态结果）不溯源不缓存。
      getattrofunc hook = ciSlotTpGetattrHook();
      if (hook != nullptr && tp->tp_getattro == hook &&
          PyMethod_Check(res) && PyMethod_GET_SELF(res) == obj.get()) {
        BorrowedRef<> func{PyMethod_GET_FUNCTION(res)};
        BorrowedRef<> descr = _PyType_Lookup(tp, name);
        if (descr == func && PyFunction_Check(descr.get())) {
          fill(tp, descr, name);
        }
      }
#endif
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kWrongTpGetAttro);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    return {nullptr, nullptr};
  } else if (_PyType_GetDict(tp) == nullptr && PyType_Ready(tp) < 0) {
    return {nullptr, nullptr};
  }

  descr = _PyType_Lookup(tp, name);
  if (descr != nullptr) {
    Py_INCREF(descr);
    if (PyFunction_Check(descr) || Py_TYPE(descr) == &PyMethodDescr_Type ||
        PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
      is_method = true;
    } else {
      f = descr->ob_type->tp_descr_get;
      if (f != nullptr && PyDescr_IsData(descr)) {
        maybeCollectCacheStats(
            cache_stats_, tp, name, CacheMissReason::kPyDescrIsData);
        PyObject* result = f(descr, obj, (PyObject*)obj->ob_type);
        Py_DECREF(descr);
        if (result == nullptr) {
          return {nullptr, nullptr};
        }
        Py_INCREF(Py_None);
        return {Py_None, result};
      }
    }
  }

#if PY_VERSION_HEX < 0x030C0000
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    // 无副作用直读（_PyObject_GetDictPtr 会物化 values 形态实例，
    // 方法慢路径高频，物化副作用曾把新生实例批量转入慢形态）；
    // hint 与实例属性方法位共用（同名同槽）。
    attr = ci_peek_instance_attr_hinted_311(obj, name, &ia_hint_);
    if (attr != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kUncategorized);
      // 记录实例属性方法位（本分支可达即类侧无数据描述符遮蔽；该
      // 前提由 tp_version_tag 钉住，类侧任何变化经版本失效）。
      if (ensureVersionTag(tp)) {
        ia_type_ = tp;
        ia_type_version_ = tp->tp_version_tag;
        ia_hint_ = -1;
      }
      Py_INCREF(attr);
      Py_XDECREF(descr);
      Py_INCREF(Py_None);
      return {Py_None, attr};
    }
  } else {
    dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr != nullptr && (dict = *dictptr) != nullptr) {
      Py_INCREF(dict);
      attr = PyDict_GetItem(dict, name);
      if (attr != nullptr) {
        maybeCollectCacheStats(
            cache_stats_, tp, name, CacheMissReason::kUncategorized);
        Py_INCREF(attr);
        Py_DECREF(dict);
        Py_XDECREF(descr);
        Py_INCREF(Py_None);
        return {Py_None, attr};
      }
      Py_DECREF(dict);
    }
  }
#else
  dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr && (dict = *dictptr) != nullptr) {
    Py_INCREF(dict);
    attr = PyDict_GetItem(dict, name);
    if (attr != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kUncategorized);
      Py_INCREF(attr);
      Py_DECREF(dict);
      Py_XDECREF(descr);
      Py_INCREF(Py_None);
      return {Py_None, attr};
    }
    Py_DECREF(dict);
  }
#endif

  if (is_method) {
    fill(tp, descr, name);
    Py_INCREF(obj);
    return {descr, obj};
  }

  if (f != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    PyObject* result = f(descr, obj, (PyObject*)Py_TYPE(obj));
    Py_DECREF(descr);
    if (result == nullptr) {
      return {nullptr, nullptr};
    }
    Py_INCREF(Py_None);
    return {Py_None, result};
  }

  if (descr != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, descr};
  }

  raise_attribute_error(obj, name);
  return {nullptr, nullptr};
}

void LoadMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    BorrowedRef<> name) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  for (auto& entry : entries_) {
    if (entry.type == nullptr) {
      uint32_t keys_version = 0;
      if (!canCacheAttribute(type, name, keys_version)) {
        break;
      }

      lm_watcher.watch(type, this);
      incICStat(g_ic_runtime_stats.lm_fill);
      entry.type = type;
      entry.value = value;
      entry.keys_version = keys_version;
#if PY_VERSION_HEX < 0x030C0000
      // 上方 Ci_Type_HasValidVersionTag 已保证此时 tag 有效。
      entry.type_version = type->tp_version_tag;
#endif
      return;
    }
  }
}

LoadTypeMethodCache::~LoadTypeMethodCache() {
  if (type_ != nullptr) {
    ltm_watcher.unwatch(type_, this);
  }
}

LoadMethodResult LoadTypeMethodCache::lookupHelper(
    LoadTypeMethodCache* cache,
    PyTypeObject* obj,
    PyObject* name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadTypeMethodCache::getValueHelper(
    LoadTypeMethodCache* cache,
    PyObject* obj) {
  PyObject* result = cache->value_;
  Py_INCREF(result);
  if (cache->is_unbound_meth_) {
    Py_INCREF(obj);
    return {result, obj};
  }
  Py_INCREF(Py_None);
  return {Py_None, result};
}

// This needs to be kept in sync with PyType_Type.tp_getattro.
LoadMethodResult LoadTypeMethodCache::lookup(
    BorrowedRef<PyTypeObject> obj,
    BorrowedRef<> name) {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 不发射内联类型比较快路径（无 type watcher，槽对无法拉式验证），
  // 命中判定在此完成：类型指针与 tp_version_tag 双重校验（D5），校验
  // 通过前不使用缓存值（D9）。
  if (type_ == obj && value_ != nullptr &&
      Ci_Type_HasValidVersionTag(type_) &&
      type_->tp_version_tag == version_) {
    return getValueHelper(this, obj.getObj());
  }
#endif
  PyTypeObject* metatype = Py_TYPE(obj);
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kWrongTpGetAttro);
    PyObject* res = PyObject_GetAttr(obj, name);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }
  if (_PyType_GetDict(obj) == nullptr) {
    if (PyType_Ready(obj) < 0) {
      return {nullptr, nullptr};
    }
  }

  descrgetfunc meta_get = nullptr;
  PyObject* meta_attribute = _PyType_Lookup(metatype, name);
  if (meta_attribute != nullptr) {
    Py_INCREF(meta_attribute);
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;

    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      /* Data descriptors implement tp_descr_set to intercept
       * writes. Assume the attribute is not overridden in
       * type's tp_dict (and bases): call the descriptor now.
       */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kPyDescrIsData);
      PyObject* res =
          meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
      Py_DECREF(meta_attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
  }

  /* No data descriptor found on metatype. Look in tp_dict of this
   * type and its bases */
  PyObject* attribute = _PyType_Lookup(obj, name);
  if (attribute != nullptr) {
    Py_XDECREF(meta_attribute);
    BorrowedRef<PyTypeObject> attribute_type = Py_TYPE(attribute);
    if (attribute_type == &PyClassMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyClassMethod_GetFunc(attribute);
      if (Py_TYPE(cm_callable) == &PyFunction_Type) {
        Py_INCREF(obj);
        Py_INCREF(cm_callable);

        // Get the underlying callable from classmethod and return the
        // callable alongside the class object, allowing the runtime to call
        // the method as an unbound method.
        fill(obj, cm_callable, true);
        return {cm_callable, obj};
      } else if (Py_TYPE(cm_callable)->tp_descr_get != nullptr) {
        // cm_callable has custom tp_descr_get that can run arbitrary
        // user code. Do not cache in this instance.
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        Py_INCREF(Py_None);
        return {
            Py_None, Py_TYPE(cm_callable)->tp_descr_get(cm_callable, obj, obj)};
      } else {
        // It is not safe to cache custom objects decorated with classmethod
        // as they can be modified later
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        BorrowedRef<> py_meth = PyMethod_New(cm_callable, obj);
        Py_INCREF(Py_None);
        return {Py_None, py_meth};
      }
    }
    if (attribute_type == &PyStaticMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyStaticMethod_GetFunc(attribute);
      Py_INCREF(cm_callable);
      Py_INCREF(Py_None);
      fill(obj, cm_callable, false);
      return {Py_None, cm_callable};
    }
    if (PyFunction_Check(attribute)) {
      Py_INCREF(attribute);
      Py_INCREF(Py_None);
      fill(obj, attribute, false);
      return {Py_None, attribute};
    }
    Py_INCREF(attribute);
    /* Implement descriptor functionality, if any */
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;
    if (local_get != nullptr) {
      /* nullptr 2nd argument indicates the descriptor was
       * found on the target object itself (or a base)  */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kUncategorized);
      PyObject* res = local_get(attribute, nullptr, obj);
      Py_DECREF(attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, attribute};
  }

  /* No attribute found in local __dict__ (or bases): use the
   * descriptor from the metatype, if any */
  if (meta_get != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    PyObject* res;
    res = meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
    Py_DECREF(meta_attribute);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }

  /* If an ordinary attribute was found on the metatype, return it now */
  if (meta_attribute != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, meta_attribute};
  }

  raise_attribute_error(obj, name);
  return {nullptr, nullptr};
}

PyTypeObject** LoadTypeMethodCache::typeAddr() {
  return &type_;
}

BorrowedRef<> LoadTypeMethodCache::value() {
  return value_;
}

void LoadTypeMethodCache::typeChanged(BorrowedRef<PyTypeObject> /* type */) {
  type_ = nullptr;
  value_.reset();
}

void LoadTypeMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadTypeMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadTypeMethodCache::cacheStats() {
  return cache_stats_.get();
}

void LoadTypeMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    bool is_unbound_meth) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  ltm_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
  is_unbound_meth_ = is_unbound_meth;
#if PY_VERSION_HEX < 0x030C0000
  // 上方 Ci_Type_HasValidVersionTag 已保证此时 tag 有效。
  version_ = type->tp_version_tag;
#endif
  ltm_watcher.watch(type_, this);
}

PyObject* LoadModuleAttrCache::lookupHelper(
    LoadModuleAttrCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

static BorrowedRef<PyDictObject> getModuleDict(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return mod->md_dict;
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return mod->globals;
  }
  return nullptr;
}

PyObject* LoadModuleAttrCache::lookup(
    BorrowedRef<> object,
    BorrowedRef<> name) {
  // First, check if we can use the cached value. If we can, we will return a
  // new reference to it.
#if PY_VERSION_HEX >= 0x030E0000
  if (module_ == object && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return Py_NewRef(res);
    }
  }
#else
  if (module_ == object && value_ != nullptr &&
      version_ == getModuleVersion(object)) {
    return Py_NewRef(value_);
  }
#endif

  // Otherwise, we will fall back to the slow path.
  return lookupSlowPath(object, name);
}

static std::pair<ci_dict_version_tag_t, PyObject*> getModuleAttribute(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  BorrowedRef<PyDictObject> dict = getModuleDict(obj);

  if (dict != nullptr &&
      (tp->tp_getattro == PyModule_Type.tp_getattro ||
       tp->tp_getattro == Ci_StrictModule_Type.tp_getattro) &&
      _PyType_Lookup(tp, name) == nullptr) {
    return {Ci_DictVersionTag(dict), PyDict_GetItemWithError(dict, name)};
  }

  return {0, nullptr};
}

PyObject* __attribute__((noinline)) LoadModuleAttrCache::lookupSlowPath(
    BorrowedRef<> object,
    BorrowedRef<> name) {
  auto [version, value] = getModuleAttribute(object, name);

  if (value != nullptr) {
#if PY_VERSION_HEX >= 0x030E0000
    PyObject* dict = getModuleDict(object);
    BorrowedRef<PyUnicodeObject> uname{name};
    if (hasOnlyUnicodeKeys(dict)) {
      cache_ = cinderx::getModuleState()->cache_manager->getGlobalCache(
          dict, dict, uname);
    }
#else
    value_ = value;
    version_ = version;
#endif
    module_ = object;

    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    return Py_NewRef(value);
  }

  auto generic = Ref<>::steal(PyObject_GetAttr(object, name));
  return generic == nullptr ? nullptr : generic.release();
}

#if PY_VERSION_HEX < 0x030C0000

PyObject* AttributeCache::siteExtGetAttr(PyObject* obj) {
  switch (site_kind_) {
    case SiteExtKind::kModule:
      if (obj == site_container_ && site_value_ != nullptr &&
          site_version_ == getModuleVersion(BorrowedRef<>{obj})) {
        incICStat(g_ic_runtime_stats.la_site_module_hit);
        return Py_NewRef(site_value_);
      }
      break;
    case SiteExtKind::kTypeAttr:
      if (obj == site_container_ && site_value_ != nullptr) {
        auto tp = reinterpret_cast<PyTypeObject*>(obj);
        if (Ci_Type_HasValidVersionTag(tp) &&
            tp->tp_version_tag == site_version_) {
          incICStat(g_ic_runtime_stats.la_site_type_hit);
          return Py_NewRef(site_value_);
        }
      }
      break;
    case SiteExtKind::kNone:
      break;
  }
  return nullptr;
}

void AttributeCache::siteExtTryFill(
    PyObject* obj,
    PyObject* name,
    PyObject* result) {
  if (site_kind_ != SiteExtKind::kNone) {
    // 单槽：已被占据。版本失效的旧形态由 siteExtGetAttr 未命中自然
    // 落到本函数，允许同容器重填；异容器（站点多态）不抢占。
    if (obj != site_container_) {
      return;
    }
  }
  if (PyModule_CheckExact(obj) || Ci_StrictModule_Check(obj)) {
    auto [version, value] = getModuleAttribute(obj, name);
    // 仅当泛型协议的返回与模块 dict 中的对象同一时才可缓存（排除
    // __getattr__ 钩子、shadow 于类型侧的属性等变换形态）。
    if (value != nullptr && value == result) {
      site_container_ = obj;
      site_value_ = value;
      site_version_ = version;
      site_kind_ = SiteExtKind::kModule;
    }
    return;
  }
  // 类型接收者不再走单槽 siteExt（多态受者单槽即失效），改由
  // typeRecv* 多条目缓存承接（劣化归因轮 C1）；kTypeAttr 枚举值保留
  // 以兼容既有槽位状态。
}

PyObject* AttributeCache::typeRecvGetAttr(PyObject* obj) {
  if (type_recv_ == nullptr) {
    return nullptr;
  }
  auto tp = reinterpret_cast<PyTypeObject*>(obj);
  PyTypeObject* metatype = Py_TYPE(obj);
  for (auto& e : type_recv_->entries) {
    if (e.form == TypeRecvEntry::Form::kNone) {
      continue;
    }
    if (e.form == TypeRecvEntry::Form::kValue) {
      if (e.recv != tp) {
        continue;
      }
      // 元类型版本校验（元类型侧无新增数据描述符遮蔽）+ 受者版本
      // 校验（其 MRO 任意层变更经 PyType_Modified 传播）。失效即
      // 清槽（版本单调，该条目不可能再命中；清槽期间不触碰借引用，
      // D9）。
      if (!Ci_Type_HasValidVersionTag(metatype) ||
          metatype->tp_version_tag != e.meta_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      if (!Ci_Type_HasValidVersionTag(tp) ||
          tp->tp_version_tag != e.recv_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      incICStat(g_ic_runtime_stats.la_site_type_hit);
      return Py_NewRef(e.payload);
    }
    // kMetaDescr：按元类型键控（同名数据描述符对该元类型的一切类
    // 通用），受者无关；元类型版本钉住描述符身份，数据描述符优先级
    // 不受受者字典影响，受者版本无需校验。
    if (e.recv != metatype) {
      continue;
    }
    if (!Ci_Type_HasValidVersionTag(metatype) ||
        metatype->tp_version_tag != e.meta_version) {
      e = TypeRecvEntry{};
      return nullptr;
    }
    incICStat(g_ic_runtime_stats.la_site_type_hit);
    descrgetfunc get = Py_TYPE(e.payload)->tp_descr_get;
    return get(e.payload, obj, reinterpret_cast<PyObject*>(metatype));
  }
  return nullptr;
}

void AttributeCache::typeRecvTryFill(
    PyObject* obj,
    PyObject* name,
    PyObject* result) {
  auto tp = reinterpret_cast<PyTypeObject*>(obj);
  PyTypeObject* metatype = Py_TYPE(obj);
  // 标准 type_getattro 可复刻；带 __getattr__ 的元类型（slot hook，
  // 如 EnumType——枚举成员访问 TokenType.BREAK 形态，sqlglot 56 万
  // 次/值）命中侧同样可复刻：hook 前半即 type_getattro，类 dict
  // 命中即返回、不进 __getattr__。本缓存 miss 返回 nullptr 自然
  // 回落完整协议（含 __getattr__ 兜底），无"确定 miss 抛错"路径，
  // 语义安全。其余自定义元类型 getattro 不缓存。
  if ((metatype->tp_getattro != PyType_Type.tp_getattro &&
       (ciSlotTpGetattrHook() == nullptr ||
        metatype->tp_getattro != ciSlotTpGetattrHook())) ||
      !ensureVersionTag(metatype)) {
    return;
  }
  if (type_recv_ == nullptr) {
    type_recv_ = std::make_unique<TypeRecvEntries>();
  }
  // 槽位选择：同键旧槽优先（重填），其次空槽，否则轮转替换（多态
  // 类型受者轮换是本缓存的核心场景）。kMetaDescr 以元类型为键。
  auto pick_slot = [&](PyTypeObject* key) -> TypeRecvEntry* {
    TypeRecvEntry* slot = nullptr;
    for (auto& e : type_recv_->entries) {
      if (e.recv == key) {
        return &e;
      }
      if (slot == nullptr && e.form == TypeRecvEntry::Form::kNone) {
        slot = &e;
      }
    }
    if (slot == nullptr) {
      slot = &type_recv_->entries[type_recv_->rr];
      type_recv_->rr = (type_recv_->rr + 1) % TypeRecvEntries::kNumEntries;
    }
    return slot;
  };

  BorrowedRef<> meta_attr = _PyType_Lookup(metatype, name);
  if (meta_attr != nullptr &&
      Py_TYPE(meta_attr.get())->tp_descr_get != nullptr &&
      PyDescr_IsData(meta_attr.get())) {
    // kMetaDescr：元类型数据描述符（getset 如 __name__/__dict__），
    // 按元类型键控——对该元类型的一切类通用。与 result 的一致性由
    // type_getattro 语义保证（数据描述符必胜）。
    *pick_slot(metatype) = TypeRecvEntry{
        metatype,
        0,
        metatype->tp_version_tag,
        TypeRecvEntry::Form::kMetaDescr,
        meta_attr.get()};
    return;
  }
  TypeRecvEntry* slot = pick_slot(tp);

  BorrowedRef<> raw = _PyType_Lookup(tp, name);
  if (raw == nullptr) {
    return;
  }
  PyObject* payload = nullptr;
  PyTypeObject* rt = Py_TYPE(raw.get());
  if (raw.get() == result) {
    // 身份稳定形态：无 __get__ 的纯类变量，或"类上访问自返"的已知
    // 描述符类型（函数/wrapper/method 描述符——get(attr, NULL, type)
    // 恒返自身）。自定义描述符即使本次自返也不收（可能有状态）。
    if (rt->tp_descr_get == nullptr || rt == &PyFunction_Type ||
        rt == &PyWrapperDescr_Type || rt == &PyMethodDescr_Type) {
      payload = raw.get();
    }
  } else if (
      rt == &PyStaticMethod_Type &&
      Ci_PyStaticMethod_GetFunc(raw.get()) == result) {
    // staticmethod 解包稳定（payload 存底层函数，寿命随 staticmethod
    // 对象在受者字典中的存活，由受者版本钉住）。
    payload = result;
  }
  if (payload == nullptr || !ensureVersionTag(tp)) {
    return;
  }
  *slot = TypeRecvEntry{
      tp,
      tp->tp_version_tag,
      metatype->tp_version_tag,
      TypeRecvEntry::Form::kValue,
      payload};
}

#endif // PY_VERSION_HEX < 0x030C0000

LoadMethodResult LoadModuleMethodCache::lookupHelper(
    LoadModuleMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadModuleMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
#if PY_VERSION_HEX >= 0x030E0000
  if (module_obj_ == obj && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return {Py_None, Py_NewRef(res)};
    }
  }
#else
  BorrowedRef<PyDictObject> dict = getModuleDict(obj);
  ci_dict_version_tag_t version = Ci_DictVersionTag(dict);

  if (module_obj_ == obj && value_ != nullptr && module_version_ == version) {
    return {Py_None, Py_NewRef(value_)};
  }
#endif

  return lookupSlowPath(obj, name);
}

BorrowedRef<> LoadModuleMethodCache::moduleObj() {
  return module_obj_;
}

#if PY_VERSION_HEX < 0x030E0000
BorrowedRef<> LoadModuleMethodCache::value() {
  return value_;
}
#endif

LoadMethodResult __attribute__((noinline))
LoadModuleMethodCache::lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name) {
  auto [version, res] = getModuleAttribute(obj, name);

  if (res != nullptr) {
    if (PyFunction_Check(res) || PyCFunction_Check(res) ||
        Py_TYPE(res) == &PyMethodDescr_Type) {
      module_obj_ = obj;
#if PY_VERSION_HEX >= 0x030E0000
      BorrowedRef<PyUnicodeObject> uname{name};
      cache_ = cinderx::getModuleState()->cache_manager->getGlobalCache(
          getModuleDict(obj), getModuleDict(obj), uname);
#else
      module_version_ = version;
      value_ = res;
#endif
    }
    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    return {Py_None, Py_NewRef(res)};
  }
  auto generic_res = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (generic_res != nullptr) {
    return {Py_None, generic_res.release()};
  }
  return {nullptr, nullptr};
}

#if PY_VERSION_HEX < 0x030C0000
void warmSlotTpGetattrHookProbe() {
  ciSlotTpGetattrHook();
}
#endif

void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type) {
  ac_watcher.typeChanged(type);
  ac_descr_watcher.typeChanged(
      type, [](AttributeCache* cache, PyTypeObject* tp) {
        cache->descrTypeChanged(tp);
      });
  ltac_watcher.typeChanged(type);
  lm_watcher.typeChanged(type);
  ltm_watcher.typeChanged(type);
}

} // namespace jit
