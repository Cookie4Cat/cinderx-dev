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

PyObject* __attribute__((noinline)) raise_attribute_error(
    PyObject* obj,
    PyObject* name) {
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
  if (!getConfig().collect_attr_cache_stats) {
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
        size++;
        size_ptr[-size] = static_cast<uint8_t>(val_offset);
        *size_ptr = size;
        Py_INCREF(value);
        values->values[val_offset] = value;
        return 0;
      }
    }
  }
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
    // 实例字典已物化，回退通用协议（正确优先；物化实例为少数形态）。
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
}

void AttributeMutator::set_split(
    PyTypeObject* type,
    Py_ssize_t val_offset,
    [[maybe_unused]] PyDictKeysObject* keys,
    bool inline_values) {
  set_type(type, inline_values ? Kind::kSplitInline : Kind::kSplit);
  split_.val_offset = val_offset;
  split_.keys = keys;
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
      return entry.getAttr(obj, name);
    }
    // 类型指针相同但版本失效：该条目不可能再次命中（版本号单调递增），
    // 立即清空释放槽位；期间不触碰条目内的借引用（D9）。
    entry.reset();
    break;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (PyObject* result = siteExtGetAttr(obj)) {
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
  else {
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
        continue;
      }

      incICStat(g_ic_runtime_stats.lm_scan_hit);
      PyObject* result = entry.value;
      Py_INCREF(result);
      Py_INCREF(obj);
      return {result, obj};
    }
  }

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
  if (PyType_Check(obj)) {
    auto tp = reinterpret_cast<PyTypeObject*>(obj);
    PyObject* value = _PyType_Lookup(tp, name);
    // 仅纯类变量：解析值无 __get__ 且泛型协议返回原对象（排除元类
    // 数据描述符、classmethod/staticmethod/函数等描述符形态）。
    if (value != nullptr && value == result &&
        Py_TYPE(value)->tp_descr_get == nullptr && ensureVersionTag(tp)) {
      site_container_ = obj;
      site_value_ = value;
      site_version_ = tp->tp_version_tag;
      site_kind_ = SiteExtKind::kTypeAttr;
    }
  }
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
