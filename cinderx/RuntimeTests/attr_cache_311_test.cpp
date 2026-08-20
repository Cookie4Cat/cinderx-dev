// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030B0000 || PY_VERSION_HEX >= 0x030C0000
// The pull-validated cache arms under test exist only on CPython 3.11.
#else

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <memory>
#include <new>

namespace {

struct CacheDeleter {
  void operator()(jit::LoadAttrCache* cache) const {
    cache->~LoadAttrCache();
    PyMem_Free(cache);
  }
};

std::unique_ptr<jit::LoadAttrCache, CacheDeleter> makeLoadAttrCache() {
  void* mem = PyMem_Calloc(1, jit::AttributeCacheSizeTrait::size());
  JIT_CHECK(mem != nullptr, "Failed to allocate load attr cache");
  return std::unique_ptr<jit::LoadAttrCache, CacheDeleter>{
      new (mem) jit::LoadAttrCache()};
}

} // namespace

class AttrCache311Test : public RuntimeTest {};

TEST_F(AttrCache311Test, PullCheckRetiresStaleEntriesOnTypeMutation) {
  const char* src = R"(
class P:
    def __init__(self, x):
        self.x = x

inst = P(3)
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> klass = PyDict_GetItemString(globals, "P");
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(klass, nullptr);
  ASSERT_NE(inst, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));

  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  uint64_t misses = stats.load_attr.misses;
  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 3);
  EXPECT_EQ(stats.load_attr.misses, misses + 1);
  EXPECT_EQ(stats.load_attr.fills, fills + 1);

  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyLong_AsLong(second), 3);
  EXPECT_EQ(stats.load_attr.hits, hits + 1);

  // Any class mutation bumps tp_version_tag; the next use must retire the
  // entry by pull, refill, and still answer correctly.
  auto marker = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyObject_SetAttrString(klass, "marker", marker), 0);
  uint64_t invalidations = stats.load_attr.invalidations;
  fills = stats.load_attr.fills;
  auto third =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(PyLong_AsLong(third), 3);
  EXPECT_EQ(stats.load_attr.invalidations, invalidations + 1);
  EXPECT_EQ(stats.load_attr.fills, fills + 1);
}

TEST_F(AttrCache311Test, CacheTrafficDoesNotMaterializeTheInstanceDict) {
  const char* src = R"(
class P:
    cv = "classvar"

    def __init__(self, x):
        self.x = x

inst = P(7)
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(inst, nullptr);
  ASSERT_TRUE(PyType_HasFeature(Py_TYPE(inst.get()), Py_TPFLAGS_MANAGED_DICT));
  ASSERT_NE(*_PyObject_ValuesPointer(inst.get()), nullptr);
  ASSERT_EQ(*_PyObject_ManagedDictPointer(inst.get()), nullptr);

  auto x_name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cv_name = Ref<>::steal(PyUnicode_InternFromString("cv"));
  auto cache_x = makeLoadAttrCache();
  auto cache_cv = makeLoadAttrCache();

  // Misses (which fill), hits, and the class-var shadow peek all run
  // against live inline values; none of them may convert the values into
  // a real dict.
  for (int i = 0; i < 4; i++) {
    auto x = Ref<>::steal(
        jit::LoadAttrCache::invoke(cache_x.get(), inst, x_name));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(PyLong_AsLong(x), 7);
    auto cv = Ref<>::steal(
        jit::LoadAttrCache::invoke(cache_cv.get(), inst, cv_name));
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(PyUnicode_CompareWithASCIIString(cv, "classvar"), 0);
  }

  EXPECT_NE(*_PyObject_ValuesPointer(inst.get()), nullptr)
      << "cache traffic dropped the inline values";
  EXPECT_EQ(*_PyObject_ManagedDictPointer(inst.get()), nullptr)
      << "cache traffic materialized the instance dict";
}

TEST_F(AttrCache311Test, KeysVersionAllocatorIssuesFromTheUpperRange) {
  const char* src = R"(
class A:
    def __init__(self):
        self.a = 1

class B:
    def __init__(self):
        self.b = 1

a = A()
b = B()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<PyTypeObject> a_type{PyDict_GetItemString(globals, "A")};
  BorrowedRef<PyTypeObject> b_type{PyDict_GetItemString(globals, "B")};
  ASSERT_NE(a_type, nullptr);
  ASSERT_NE(b_type, nullptr);

  PyDictKeysObject* a_keys =
      reinterpret_cast<PyHeapTypeObject*>(a_type.get())->ht_cached_keys;
  PyDictKeysObject* b_keys =
      reinterpret_cast<PyHeapTypeObject*>(b_type.get())->ht_cached_keys;
  ASSERT_NE(a_keys, nullptr);
  ASSERT_NE(b_keys, nullptr);

  PyInterpreterState* interp = PyInterpreterState_Get();
  uint32_t a_version = dictGetKeysVersion(interp, a_keys);
  uint32_t b_version = dictGetKeysVersion(interp, b_keys);
  // The 3.11 allocator (shared with the vendored specializer) issues from
  // the top half of the 32-bit range so it can never collide with
  // libpython's private bottom-up stream; distinct keys objects get
  // distinct numbers, and re-asking is stable.
  EXPECT_GE(a_version, UINT32_C(1) << 31);
  EXPECT_GE(b_version, UINT32_C(1) << 31);
  EXPECT_NE(a_version, b_version);
  EXPECT_EQ(dictGetKeysVersion(interp, a_keys), a_version);
}

TEST_F(AttrCache311Test, ModuleMethodHitOwnsBothResultHalves) {
  auto mod = Ref<>::steal(PyModule_New("attr_cache_311_mod"));
  ASSERT_NE(mod, nullptr);
  auto builtins = Ref<>::steal(PyImport_ImportModule("builtins"));
  ASSERT_NE(builtins, nullptr);
  auto abs_fn = Ref<>::steal(PyObject_GetAttrString(builtins, "abs"));
  ASSERT_NE(abs_fn, nullptr);
  ASSERT_TRUE(PyCFunction_Check(abs_fn.get()));
  ASSERT_EQ(PyObject_SetAttrString(mod, "f", abs_fn), 0);
  auto name = Ref<>::steal(PyUnicode_InternFromString("f"));

  jit::LoadModuleMethodCache cache;
  // Prime: slow path fills the version-validated entry.  On 3.11 the
  // result maps {none_or_callable, inst_or_callable} onto
  // {callable, self_or_null} directly.
  auto first = cache.lookupHelper(&cache, mod, name);
  ASSERT_NE(first.self_or_null, nullptr);
  Py_XDECREF(first.callable);
  Py_XDECREF(first.self_or_null);

  // 3.11's None is mortal: a hit that returned a borrowed None would
  // decrement it to death over enough iterations.  Both halves of the
  // result must be owned.
  Py_ssize_t none_refcount = Py_REFCNT(Py_None);
  for (int i = 0; i < 64; i++) {
    auto hit = cache.lookupHelper(&cache, mod, name);
    ASSERT_EQ(hit.callable, Py_None);
    ASSERT_EQ(hit.self_or_null, abs_fn.get());
    Py_DECREF(hit.callable);
    Py_DECREF(hit.self_or_null);
  }
  EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);
}

#endif
