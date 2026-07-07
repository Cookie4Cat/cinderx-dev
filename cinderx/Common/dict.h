// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

// This needs to come before borrowed.h
#include "pycore_dict.h"

#ifdef __cplusplus
#include "cinderx/Common/ref.h"
#endif

#include "cinderx/UpstreamBorrow/borrowed.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICT_VALUES(dict) dict->ma_values->values
#include "internal/pycore_dict.h"

static inline PyObject* getBorrowedTypeDict(PyTypeObject* self) {
#if PY_VERSION_HEX < 0x030C0000
  return self->tp_dict;
#else
  return _PyType_GetDict(self);
#endif
}

#if PY_VERSION_HEX < 0x030C0000
#define _PyDict_NotifyEvent(EVENT, MP, KEY, VAL) DICT_NEXT_VERSION()
#else
#define _PyDict_NotifyEvent(EVENT, MP, KEY, VAL) \
  _PyDict_NotifyEvent(_PyInterpreterState_GET(), (EVENT), (MP), (KEY), (VAL))
#endif

// Check if a dictionary is guaranteed to only contain unicode/string keys.
//
// Does not scan the dictionary, so if internally the dictionary is a
// "general-purpose" kind but happens to only contain strings this will still
// return false.
static inline bool hasOnlyUnicodeKeys(PyObject* dict) {
  assert(PyDict_Check(dict));

  return DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys);
}

// 变宽索引槽读取（dk_indices 按 DK_SIZE 选 1/2/4/8 字节宽）。CPython 的
// dictkeys_get_index 为 dictobject.c 静态函数不可链接，按同布局复刻。
static inline Py_ssize_t dictKeysGetIndexSlot(
    const PyDictKeysObject* keys,
    size_t i) {
  int log2size = DK_LOG_SIZE(keys);
  if (log2size < 8) {
    return ((const int8_t*)keys->dk_indices)[i];
  }
  if (log2size < 16) {
    return ((const int16_t*)keys->dk_indices)[i];
  }
#if SIZEOF_VOID_P > 4
  if (log2size >= 32) {
    return ((const int64_t*)keys->dk_indices)[i];
  }
#endif
  return ((const int32_t*)keys->dk_indices)[i];
}

static inline Py_ssize_t getDictKeysIndex(
    PyDictKeysObject* keys,
    PyObject* name) {
#if PY_VERSION_HEX >= 0x030E0000
  return _PyDictKeys_StringLookupSplit(keys, name);
#else
  // unicode 键表哈希探测（CPython unicodekeys_lookup_unicode 不可链接，
  // 按 3.11 布局复刻：驻留名指针等值快判 + 哈希预判 + 扰动开放寻址；
  // 删除槽为 DKIX_DUMMY 继续探测）。此前实现为逐条目线性扫，物化大
  // 字典受者下 IC 慢路径呈 O(键数)——sqlalchemy 编译净效应 −26% 的
  // 主体（M10 sqla 净效应轮）。
  if (!DK_IS_UNICODE(keys)) {
    return -1;
  }
  Py_hash_t hash = ((PyASCIIObject*)name)->hash;
  if (hash == -1) {
    hash = PyObject_Hash(name);
  }
  size_t mask = (size_t)(DK_SIZE(keys) - 1);
  size_t perturb = (size_t)hash;
  size_t i = (size_t)hash & mask;
  for (;;) {
    Py_ssize_t ix = dictKeysGetIndexSlot(keys, i);
    if (ix >= 0) {
      PyDictUnicodeEntry* ep = &DK_UNICODE_ENTRIES(keys)[ix];
      if (ep->me_key == name) {
        return ix;
      }
      if (ep->me_key != NULL &&
          ((PyASCIIObject*)ep->me_key)->hash == hash &&
          PyUnicode_Compare(name, ep->me_key) == 0) {
        return ix;
      }
    } else if (ix == DKIX_EMPTY) {
      return -1;
    }
    perturb >>= 5; // PERTURB_SHIFT（dictobject.c）
    i = mask & (i * 5 + perturb + 1);
  }
#endif
}

// We can't borrow this from CPython because it exists but is not
// exported, and therefore borrowing it duplicates the symbol.
static inline uint32_t dictGetKeysVersion(
    PyInterpreterState* interp,
    PyDictKeysObject* dictkeys) {
  if (dictkeys->dk_version != 0) {
    return dictkeys->dk_version;
  }
  (void)interp;
#if PY_VERSION_HEX < 0x030C0000
  static uint32_t next_keys_version = 1;
  uint32_t v = next_keys_version++;
  if (v == 0) {
    return 0;
  }
#else
  if (interp->dict_state.next_keys_version == 0) {
    return 0;
  }
  uint32_t v = interp->dict_state.next_keys_version++;
#endif
  dictkeys->dk_version = v;
  return v;
}

#if PY_VERSION_HEX >= 0x030E0000
typedef uint32_t ci_dict_version_tag_t;
static inline ci_dict_version_tag_t Ci_DictVersionTag(PyDictObject* dict) {
  return _PyDict_GetKeysVersionForCurrentState(_PyInterpreterState_GET(), dict);
}
#else
typedef uint64_t ci_dict_version_tag_t;
static inline ci_dict_version_tag_t Ci_DictVersionTag(PyDictObject* dict) {
  return dict->ma_version_tag;
}
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

inline Ref<> getDictRef(PyObject* dict, PyObject* key) {
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* res;
  if (PyDict_GetItemRef(dict, key, &res) > 0) {
    return Ref<>::steal(res);
  }
#else
  PyObject* res = PyDict_GetItemWithError(dict, key);
  if (res != nullptr) {
    return Ref<>::create(res);
  }
#endif
  return nullptr;
}

#endif
