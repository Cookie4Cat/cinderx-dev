// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>

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

struct StoreCacheDeleter {
  void operator()(jit::StoreAttrCache* cache) const {
    cache->~StoreAttrCache();
    PyMem_Free(cache);
  }
};

std::unique_ptr<jit::StoreAttrCache, StoreCacheDeleter> makeStoreAttrCache() {
  void* mem = PyMem_Calloc(1, jit::AttributeCacheSizeTrait::size());
  JIT_CHECK(mem != nullptr, "Failed to allocate store attr cache");
  return std::unique_ptr<jit::StoreAttrCache, StoreCacheDeleter>{
      new (mem) jit::StoreAttrCache()};
}

// A data descriptor whose slots delete the descriptor from its owner
// class mid-call (dropping what may be its last reference) and then keep
// using their own storage.  Stock survives this because
// GenericGetAttr/GenericSetAttr hold a strong reference across the slot
// call; the cached dispatch must provide the same ownership.  The
// "armed" latch lets the priming call fill the cache before the
// self-deleting call runs, and died_mid_slot turns the use-after-free
// into a Release-visible verdict (the ASAN leg gives the memory one).
struct SelfDeletingDescr {
  PyObject_HEAD long payload;
};

long sdd_dealloc_count = 0;
bool sdd_died_mid_slot = false;
bool sdd_armed = false;
PyObject* sdd_owner = nullptr; // borrowed: the owner class

void sdd_dealloc(PyObject* self) {
  sdd_dealloc_count++;
  PyObject_Free(self);
}

PyObject* sdd_descr_get(PyObject* self, PyObject*, PyObject*) {
  if (sdd_armed) {
    long before = sdd_dealloc_count;
    if (PyObject_DelAttrString(sdd_owner, "x") < 0) {
      return nullptr;
    }
    if (sdd_dealloc_count != before) {
      // `self` is already dead; touching payload would be the UAF.
      sdd_died_mid_slot = true;
      Py_RETURN_NONE;
    }
  }
  return PyLong_FromLong(reinterpret_cast<SelfDeletingDescr*>(self)->payload);
}

int sdd_descr_set(PyObject* self, PyObject*, PyObject* value) {
  if (value == nullptr) {
    PyErr_SetString(PyExc_AttributeError, "cannot delete");
    return -1;
  }
  if (sdd_armed) {
    long before = sdd_dealloc_count;
    if (PyObject_DelAttrString(sdd_owner, "x") < 0) {
      return -1;
    }
    if (sdd_dealloc_count != before) {
      sdd_died_mid_slot = true;
      return 0;
    }
  }
  reinterpret_cast<SelfDeletingDescr*>(self)->payload += 1;
  return 0;
}

PyTypeObject SelfDeletingDescr_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) //
};

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
    auto x =
        Ref<>::steal(jit::LoadAttrCache::invoke(cache_x.get(), inst, x_name));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(PyLong_AsLong(x), 7);
    auto cv =
        Ref<>::steal(jit::LoadAttrCache::invoke(cache_cv.get(), inst, cv_name));
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

  // 3.11's None is mortal: any arm that returned a borrowed None would
  // decrement it to death over enough iterations, so every arm -- cold
  // slow path, hit, non-function value, generic fallback -- runs 64
  // rounds against a refcount baseline.
  //
  // Absolute refcount baselines are order-sensitive in a full-suite run:
  // GC of prior tests' garbage and one-time lazy initialization inside
  // the first lookup of a given shape both move the count for reasons
  // that are not per-call ownership bugs.  So: GC is drained and held
  // off (RAII so a failing ASSERT cannot leak the disabled state), and
  // each measured block runs ONE priming round before its baseline --
  // a per-call borrow still shows up as a full -64.
  struct GcOff {
    GcOff() {
      PyGC_Collect();
      PyGC_Disable();
    }
    ~GcOff() {
      PyGC_Enable();
    }
  } gc_off;

  auto run_round = [&](BorrowedRef<> lookup_name, PyObject* expected_value) {
    jit::LoadModuleMethodCache cold;
    auto res = cold.lookupHelper(&cold, mod, lookup_name);
    ASSERT_EQ(res.callable, Py_None);
    if (expected_value != nullptr) {
      ASSERT_EQ(res.self_or_null, expected_value);
    } else {
      ASSERT_NE(res.self_or_null, nullptr);
    }
    Py_DECREF(res.callable);
    Py_DECREF(res.self_or_null);
  };

  {
    // Cold fill plus hits on one cache: prime, baseline, then measure
    // the slow-path fill and 64 hits together.
    jit::LoadModuleMethodCache prime;
    auto first = prime.lookupHelper(&prime, mod, name);
    ASSERT_EQ(first.callable, Py_None);
    ASSERT_EQ(first.self_or_null, abs_fn.get());
    Py_DECREF(first.callable);
    Py_DECREF(first.self_or_null);

    Py_ssize_t none_refcount = Py_REFCNT(Py_None);
    jit::LoadModuleMethodCache cache;
    for (int i = 0; i < 64; i++) {
      auto hit = cache.lookupHelper(&cache, mod, name);
      ASSERT_EQ(hit.callable, Py_None);
      ASSERT_EQ(hit.self_or_null, abs_fn.get());
      Py_DECREF(hit.callable);
      Py_DECREF(hit.self_or_null);
    }
    // Fresh caches per round keep every iteration on the cold slow path.
    for (int i = 0; i < 64; i++) {
      run_round(name, abs_fn.get());
    }
    EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);
  }

  // A non-function value in the module dict returns through the same
  // slow-path arm without filling the cache; its None half must be owned
  // as well.
  auto value = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttrString(mod, "v", value), 0);
  auto vname = Ref<>::steal(PyUnicode_InternFromString("v"));
  run_round(vname, value.get());
  Py_ssize_t none_refcount = Py_REFCNT(Py_None);
  for (int i = 0; i < 64; i++) {
    run_round(vname, value.get());
  }
  EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);

  // A name absent from the module dict resolves through the
  // PyObject_GetAttr generic arm (here: a type attribute); that return
  // must own its None half too.
  auto sname = Ref<>::steal(PyUnicode_InternFromString("__str__"));
  run_round(sname, nullptr);
  none_refcount = Py_REFCNT(Py_None);
  for (int i = 0; i < 64; i++) {
    run_round(sname, nullptr);
  }
  EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);
}

TEST_F(AttrCache311Test, DescriptorSurvivesDeletingItselfMidSlot) {
  if (SelfDeletingDescr_Type.tp_name == nullptr) {
    SelfDeletingDescr_Type.tp_name = "SelfDeletingDescr";
    SelfDeletingDescr_Type.tp_basicsize = sizeof(SelfDeletingDescr);
    SelfDeletingDescr_Type.tp_dealloc = sdd_dealloc;
    SelfDeletingDescr_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    SelfDeletingDescr_Type.tp_descr_get = sdd_descr_get;
    SelfDeletingDescr_Type.tp_descr_set = sdd_descr_set;
    ASSERT_GE(PyType_Ready(&SelfDeletingDescr_Type), 0);
  }

  auto run_arm = [&](bool is_store) {
    const char* src = R"(
class C:
    pass

inst = C()
)";
    Ref<PyObject> globals(MakeGlobals());
    ASSERT_NE(globals, nullptr);
    auto result =
        Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
    ASSERT_NE(result, nullptr);
    BorrowedRef<> klass = PyDict_GetItemString(globals, "C");
    BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
    ASSERT_NE(klass, nullptr);
    ASSERT_NE(inst, nullptr);

    SelfDeletingDescr* descr =
        PyObject_New(SelfDeletingDescr, &SelfDeletingDescr_Type);
    ASSERT_NE(descr, nullptr);
    descr->payload = 42;
    // The 3.11 fill refuses descriptor types without a valid version tag
    // (the JIT never assigns one itself); an ordinary attribute lookup
    // through the type makes CPython assign it, exactly as organic use
    // would have.
    {
      auto warmed = Ref<>::steal(PyObject_GetAttrString(
          reinterpret_cast<PyObject*>(descr), "__class__"));
      ASSERT_NE(warmed, nullptr);
    }
    ASSERT_EQ(
        PyObject_SetAttrString(klass, "x", reinterpret_cast<PyObject*>(descr)),
        0);
    // The class dict now holds the ONLY reference.
    Py_DECREF(descr);

    sdd_owner = klass;
    sdd_armed = false;
    sdd_died_mid_slot = false;
    sdd_dealloc_count = 0;
    auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
    auto value = Ref<>::steal(PyLong_FromLong(7));
    const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

    if (is_store) {
      auto cache = makeStoreAttrCache();
      // Prime: fills the kDataDescr entry through the working slot.  The
      // stats assertions make a vacuous pass impossible: a refused fill
      // would leave every call on the stock-protected slow path and the
      // test would prove nothing.
      uint64_t fills = stats.store_attr.fills;
      ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, value), 0);
      ASSERT_EQ(stats.store_attr.fills, fills + 1);
      sdd_armed = true;
      // Hit: the slot deletes the descriptor's only other reference
      // mid-call; the dispatch's strong hold must keep it alive.
      uint64_t hits = stats.store_attr.hits;
      ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, value), 0);
      ASSERT_EQ(stats.store_attr.hits, hits + 1);
    } else {
      auto cache = makeLoadAttrCache();
      uint64_t fills = stats.load_attr.fills;
      auto first =
          Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
      ASSERT_NE(first, nullptr);
      EXPECT_EQ(PyLong_AsLong(first), 42);
      ASSERT_EQ(stats.load_attr.fills, fills + 1);
      sdd_armed = true;
      uint64_t hits = stats.load_attr.hits;
      auto second =
          Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
      ASSERT_NE(second, nullptr);
      EXPECT_EQ(PyLong_AsLong(second), 42);
      ASSERT_EQ(stats.load_attr.hits, hits + 1);
    }

    EXPECT_FALSE(sdd_died_mid_slot)
        << "the descriptor was deallocated while its own slot was running";
    // The dispatch guard was the last reference: the descriptor died
    // exactly once, after the slot returned.
    EXPECT_EQ(sdd_dealloc_count, 1);
    sdd_armed = false;
    sdd_owner = nullptr;
  };

  run_arm(/*is_store=*/false);
  run_arm(/*is_store=*/true);
}

#endif
