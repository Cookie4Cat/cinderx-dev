// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>

namespace jit {

// Mutator for an instance attribute that is stored in a split dictionary
struct SplitMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* getAttrKnownOffset(PyObject* obj, PyObject* name);
  int setAttrKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInline(PyObject* obj, PyObject* name);
  PyObject* getAttrSlowPath(
      PyObject* obj,
      PyObject* name,
      BorrowedRef<PyDictObject> dict);
  int setAttrInline(PyObject* obj, PyObject* name, PyObject* value);
  PyObject* getAttrInlineKnownOffset(PyObject* obj, PyObject* name);
  int setAttrInlineKnownOffset(PyObject* obj, PyObject* name, PyObject* value);
#elif PY_VERSION_HEX < 0x030C0000
  // 3.11 values 形态快路径（M9 性能归因轮）：managed dict 的预头双槽
  // 布局（-4=PyDictValues*，-3=PyDictObject*；values 槽非空即 values
  // 形态）。命中有效性由条目级 tp_version_tag 拉式校验（matches()）
  // 前置保证；3.11 共享键不增长（不够用即物化），val_offset 恒在
  // 实例 values 容量内。写侧维持通用协议（后续项）。
  PyObject* getAttrInline(PyObject* obj, PyObject* name);
  PyObject* getAttrInlineKnownOffset(PyObject* obj, PyObject* name);
#endif
  bool canInsertToSplitDict(BorrowedRef<PyDictObject> dict, BorrowedRef<> name);
  bool ensureValueOffset(BorrowedRef<> name);

  Py_ssize_t val_offset;
  PyDictKeysObject* keys; // Borrowed
#if PY_VERSION_HEX < 0x030C0000
  // 物化实例字典的 me_key 自验证 hint（任意值均安全：越界或键不符即
  // 重算），读写侧共用。
  Py_ssize_t mat_hint;
#endif
};

// Mutator for an instance attribute that is stored in a combined dictionary
struct CombinedMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  Py_ssize_t dict_offset;
};

// Mutator for a data descriptor
struct DataDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  BorrowedRef<> descr;
  BorrowedRef<PyTypeObject> descr_type;
};

// Mutator for a member descriptor
struct MemberDescrMutator {
  PyObject* getAttr(PyObject* obj);
  int setAttr(PyObject* obj, PyObject* value);

  PyMemberDef* memberdef;
};

// Attribute corresponds to a non-data descriptor or a class variable
struct DescrOrClassVarMutator {
  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  BorrowedRef<> descr;
  uint32_t keys_version;
#if PY_VERSION_HEX < 0x030C0000
  // 实例字典遮蔽检查的 me_key 自验证 hint（go 三件套①：类默认值被
  // 实例遮蔽的形态每次访问付字符串哈希字典查找）。任意值均安全。
  Py_ssize_t mat_hint;
#endif
};

// An instance of AttributeMutator is specialized to more efficiently perform a
// get/set of a particular kind of attribute.
class AttributeMutator {
 public:
  // Kind enum is designed to fit within 3 bits and it's value is embedded into
  // the type_ pointer
  enum class Kind : uint8_t {
    kSplit,
    kSplitKnownOffset,
    kSplitInline,
    kSplitInlineKnownOffset,
    kCombined,
    kDataDescr,
    kMemberDescr,
    kDescrOrClassVar,
    kMaxValue,
  };
  static_assert(
      static_cast<uint8_t>(Kind::kMaxValue) <= 8,
      "Kind enum should fit in 3 bits");

  AttributeMutator();
  PyTypeObject* type() const {
    // clear tagged bits and return
    return reinterpret_cast<PyTypeObject*>(type_ & ~kindMask());
  }

  // 命中判定。3.12+ 仅比较类型指针（type watcher 负责回调失效）；3.11 无
  // type watcher，额外以 tp_version_tag 拉式验证（D5）。验证不通过时调用
  // 方不得解引用条目内的借引用（D9）。
  bool matches(PyTypeObject* tp) const {
    if (type() != tp) {
      return false;
    }
#if PY_VERSION_HEX < 0x030C0000
    if (!Ci_Type_HasValidVersionTag(tp) ||
        tp->tp_version_tag != type_version_) {
      return false;
    }
#endif
    return true;
  }

  void reset();
  bool isEmpty() const {
    return type_ == 0;
  }
  void set_combined(PyTypeObject* type);
  void set_data_descr(PyTypeObject* type, PyObject* descr);
  void set_member_descr(PyTypeObject* type, PyObject* descr);
  void set_descr_or_classvar(
      PyTypeObject* type,
      PyObject* descr,
      uint32_t keys_version);
  void set_split(
      PyTypeObject* type,
      Py_ssize_t val_offset,
      PyDictKeysObject* keys,
      bool values_inline);
  BorrowedRef<PyTypeObject> watchedDescrType() const;

  PyObject* getAttr(PyObject* obj, PyObject* name);
  int setAttr(PyObject* obj, PyObject* name, PyObject* value);

  static void changeKindFromSplitInline(SplitMutator* split, Kind new_kind);
  static constexpr uintptr_t kindMask() {
    return 0x07;
  }
  static constexpr uintptr_t splitInlineKnownOffsetKind() {
    return static_cast<uintptr_t>(Kind::kSplitInlineKnownOffset);
  }
  static constexpr size_t typeOffset() {
    return offsetof(AttributeMutator, type_);
  }
  static constexpr size_t splitValOffsetOffset() {
    return offsetof(AttributeMutator, split_) +
        offsetof(SplitMutator, val_offset);
  }
#if PY_VERSION_HEX < 0x030C0000
  static constexpr size_t typeVersionOffset() {
    return offsetof(AttributeMutator, type_version_);
  }
  static constexpr size_t splitMatHintOffset() {
    return offsetof(AttributeMutator, split_) +
        offsetof(SplitMutator, mat_hint);
  }
  // kind-6/7（kMemberDescr/kDescrOrClassVar）内联快路径的条目布局
  // 访问器（劣化归因轮 C3；gen_asm la 桩汇编发射用）。
  static constexpr uintptr_t descrOrClassVarKind() {
    return static_cast<uintptr_t>(Kind::kDescrOrClassVar);
  }
  static constexpr uintptr_t memberDescrKind() {
    return static_cast<uintptr_t>(Kind::kMemberDescr);
  }
  static constexpr size_t memberDefOffset() {
    return offsetof(AttributeMutator, member_descr_) +
        offsetof(MemberDescrMutator, memberdef);
  }
  static constexpr size_t dcvDescrOffset() {
    return offsetof(AttributeMutator, descr_or_cvar_) +
        offsetof(DescrOrClassVarMutator, descr);
  }
  static constexpr size_t dcvKeysVersionOffset() {
    return offsetof(AttributeMutator, descr_or_cvar_) +
        offsetof(DescrOrClassVarMutator, keys_version);
  }
  static constexpr size_t dcvMatHintOffset() {
    return offsetof(AttributeMutator, descr_or_cvar_) +
        offsetof(DescrOrClassVarMutator, mat_hint);
  }
#endif

  uintptr_t kindBits() const {
    return type_ & kindMask();
  }

 private:
  void set_type(PyTypeObject* type, Kind kind);
  Kind get_kind() const {
    return static_cast<Kind>(type_ & kindMask());
  }

  uintptr_t type_; // This value stores both a PyTypeObject* for the type object
                   // and the Kind enum value which are bitpacked together to
                   // reduce memory consumption
#if PY_VERSION_HEX < 0x030C0000
  // set_type 记录时的 tp_version_tag，matches() 命中前拉式验证（D5）。
  uint32_t type_version_{0};
#endif
  union {
    SplitMutator split_;
    CombinedMutator combined_;
    DataDescrMutator data_descr_;
    MemberDescrMutator member_descr_;
    DescrOrClassVarMutator descr_or_cvar_;
  };
};

class AttributeCache {
 public:
  AttributeCache();
  ~AttributeCache();

  void typeChanged(PyTypeObject* type);
  void descrTypeChanged(PyTypeObject* type);
  static constexpr size_t entriesOffset() {
    return offsetof(AttributeCache, entries_);
  }

  // 共享桩前置（桩共享轮）：slow 尾的 IC 压力计数地址由发射期烘焙
  // 迁为 cache 字段——桩经 x0=cache 取得，发射体不再 per-code。
  static constexpr size_t pressureSlotOffset() {
    return offsetof(AttributeCache, pressure_slot_);
  }
  void setPressureSlot(uint64_t* slot) {
    pressure_slot_ = slot;
  }

 protected:
  std::span<AttributeMutator> entries() {
    return {entries_, getConfig().attr_cache_size};
  }

  AttributeMutator* findEmptyEntry();

  uint64_t* pressure_slot_{nullptr};

  void fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name);

  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name, BorrowedRef<> descr);

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 站点扩展：承接 fill 因 tp_getattro 非泛型而拒绝的两类高频
  // 接收者（IC 计数轮站点归属：模块属性如 math.sqrt、无 __get__ 的
  // 纯类变量如求解器方向/强度常量）。单槽，首个稳定形态占据。
  //  - kModule：值借引用 + 模块 dict 版本（Ci_DictVersionTag，任何
  //    字典变更即失效）拉式验证；
  //  - kTypeAttr：值借引用 + tp_version_tag（VALID 标志）拉式验证，
  //    MRO 任意层变更经 PyType_Modified 传播失效。
  // 两个版本号发号器全局单调不复用：容器对象亡后即使地址复用，指针
  // 相等而版本必不相等，借引用在版本校验通过前不被解引用（D9）。
  enum class SiteExtKind : uint8_t { kNone, kModule, kTypeAttr };

  PyObject* siteExtGetAttr(PyObject* obj);
  void siteExtTryFill(PyObject* obj, PyObject* name, PyObject* result);

  BorrowedRef<> site_container_;
  BorrowedRef<> site_value_;
  uint64_t site_version_{0};
  SiteExtKind site_kind_{SiteExtKind::kNone};

  // 类型接收者多条目缓存（劣化归因轮 C1）：受者为类型对象时通用条目
  // 按元类型键控失效、fill 又对 type_getattro 拒填，此前永久慢路径
  //（pprint 的 type(obj).__repr__ 单站点 1440 万次/值即此形态，且
  // 受者多态——dict/list/str 轮换——单槽 siteExt 无法承接）。
  // 两种可缓存形态：
  //  - kValue：解析结果与受者 MRO 原始属性同一（无 __get__ 的纯类
  //    变量；函数/wrapper/method 描述符在类上访问自返；staticmethod
  //    解包）。受者与元类型 tp_version_tag 双拉式校验。
  //  - kMetaDescr：元类型数据描述符（type.__name__ 等 getset），缓存
  //    描述符本体、命中时活调其 get；元类型版本单校验（数据描述符
  //    优先级不受受者字典影响）。
  // 借引用在版本校验通过前不被解引用（D9）；条目轮转替换以适配
  // 多态受者。惰性分配：绝大多数站点不涉类型受者，零内存税。
  struct TypeRecvEntry {
    // kValue：recv = 受者类型；kMetaDescr：recv = 元类型——同名的
    // 元类型数据描述符对该元类型的一切实例（类）通用（type.__name__
    // 的 getset 对几十个节点类是同一个），按元类型键控使单条目吸收
    // 全部多态受者（docutils 形态实测 4 槽轮转被打穿）。
    PyTypeObject* recv{nullptr}; // 仅指针比对，命中前版本校验
    uint32_t recv_version{0};
    uint32_t meta_version{0};
    enum class Form : uint8_t { kNone, kValue, kMetaDescr } form{Form::kNone};
    PyObject* payload{nullptr}; // 借引用
  };
  struct TypeRecvEntries {
    static constexpr size_t kNumEntries = 8;
    TypeRecvEntry entries[kNumEntries];
    size_t rr{0};
  };

  PyObject* typeRecvGetAttr(PyObject* obj);
  void typeRecvTryFill(PyObject* obj, PyObject* name, PyObject* result);

  std::unique_ptr<TypeRecvEntries> type_recv_;
#endif

  AttributeMutator entries_[0];
};

struct AttributeCacheSizeTrait {
  static size_t size() {
    auto base = sizeof(AttributeCache);
    auto extra = sizeof(AttributeMutator) * getConfig().attr_cache_size;
    return base + extra;
  }
};

// A cache for an individual StoreAttrCached instruction.
//
// The logic of StoreAttrCache::invoke is equivalent to PyObject_SetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class StoreAttrCache : public AttributeCache {
 public:
  StoreAttrCache() = default;

  // Return 0 on success and a negative value on failure.
  static int
  invoke(StoreAttrCache* cache, PyObject* obj, PyObject* name, PyObject* value);

 private:
  DISALLOW_COPY_AND_ASSIGN(StoreAttrCache);

  int doInvoke(PyObject* obj, PyObject* name, PyObject* value);
  int invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value);
};

// A cache for an individual LoadAttrCached instruction.
//
// The logic of LoadAttrCache::invoke is equivalent to PyObject_GetAttr,
// however, it can be specialized and accelerated depending on the kinds of
// receiver types that are seen.
class LoadAttrCache : public AttributeCache {
 public:
  LoadAttrCache() = default;

  // Returns a new reference to the value or NULL on error.
  static PyObject* invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name);

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadAttrCache);

  PyObject* doInvoke(PyObject* obj, PyObject* name);
  PyObject* invokeSlowPath(PyObject* obj, PyObject* name);
};

// A cache for LoadAttr instructions where we expect the receiver to be a type
// object.
//
// The code for loading an attribute where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// second element (the cached value) is loaded. If they are not equal,
// `invoke()` is called, which performs the full lookup and potentially fills
// the cache.
class LoadTypeAttrCache {
 public:
  LoadTypeAttrCache();
  ~LoadTypeAttrCache();

  static PyObject*
  invoke(LoadTypeAttrCache* cache, PyObject* obj, PyObject* name);

  // Get the addresses of the type and value cache entries.
  PyTypeObject** typeAddr();
  PyObject** valueAddr();

  void typeChanged(BorrowedRef<PyTypeObject> type);

 private:
  PyObject* invokeSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  void fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value);
  void reset();

  // Cached type and value, stored as raw pointers so codegen can access them by
  // address.
  PyTypeObject* type_;
  PyObject* value_;
#if PY_VERSION_HEX < 0x030C0000
  // fill 记录时的 tp_version_tag；3.11 不发射内联 [type, value] 快路径，
  // 命中判定在 invoke() 内完成并以此拉式验证（D5）。
  uint32_t version_{0};
#endif
};

#define FOREACH_CACHE_MISS_REASON(V) \
  V(WrongTpGetAttro)                 \
  V(PyDescrIsData)                   \
  V(Uncategorized)

enum class CacheMissReason {
#define DECLARE_CACHE_MISS_REASON(name) k##name,
  FOREACH_CACHE_MISS_REASON(DECLARE_CACHE_MISS_REASON)
#undef DECLARE_CACHE_MISS_REASON
};

std::string_view cacheMissReason(CacheMissReason reason);

struct CacheMiss {
  int count{0};
  CacheMissReason reason{CacheMissReason::kUncategorized};
};

struct CacheStats {
  std::string filename;
  std::string method_name;
  std::unordered_map<std::string, CacheMiss> misses;
};

// IC 快慢路径全局计数器（PYTHONJITCOLLECTINLINECACHESTATS 门控，经
// cinderjit.get_and_clear_inline_cache_stats() 的 "globals" 段导出）。
// *_stub_entries 由 aarch64 内联 stub 在计数模式下直增（计数指令仅在
// 计数模式下发射，GIL 持有期间普通读改写即可）；其余计数在 C++ helper
// 内自增。语义：stub 命中数 = stub_entries − 对应 helper 进入数。
struct ICRuntimeStats {
  uint64_t la_stub_entries{0};
  uint64_t lm_stub_entries{0};
  uint64_t sa_stub_entries{0};
  std::atomic<uint64_t> la_invoke{0};
  std::atomic<uint64_t> la_entry_hit{0};
  // 条目命中的 kind 直方图（诊断用）
  std::atomic<uint64_t> la_hit_kind[8] = {};
  std::atomic<uint64_t> la_split_values_hit{0};
  std::atomic<uint64_t> la_split_materialized{0};
  std::atomic<uint64_t> la_site_module_hit{0};
  std::atomic<uint64_t> la_site_type_hit{0};
  std::atomic<uint64_t> la_mat_hint_hit{0};
  std::atomic<uint64_t> la_slow{0};
  std::atomic<uint64_t> lm_ia_hit{0};
  std::atomic<uint64_t> lavog_calls{0};
  std::atomic<uint64_t> lavog_values_hit{0};
  std::atomic<uint64_t> lavog_generic{0};
  std::atomic<uint64_t> lm_helper{0};
  std::atomic<uint64_t> lm_scan_hit{0};
  std::atomic<uint64_t> lm_version_fail{0};
  std::atomic<uint64_t> lm_keys_fail{0};
  std::atomic<uint64_t> lm_slow{0};
  std::atomic<uint64_t> lm_fill{0};
  std::atomic<uint64_t> sa_invoke{0};
  std::atomic<uint64_t> sa_entry_hit{0};
  // 写侧条目命中的 kind 直方图（诊断用，与 la_hit_kind 同构）
  std::atomic<uint64_t> sa_hit_kind[8] = {};
  std::atomic<uint64_t> sa_slow{0};
  // 写侧 helper 分型（插入形轮）：values 覆写/values 插入/物化覆写/
  // 通用协议回退，用于定位 stub 漏接的形态构成。
  std::atomic<uint64_t> sa_values_overwrite{0};
  std::atomic<uint64_t> sa_values_insert{0};
  std::atomic<uint64_t> sa_mat_overwrite{0};
  std::atomic<uint64_t> sa_generic_fallback{0};
};

extern ICRuntimeStats g_ic_runtime_stats;

// la_slow 站点归属直方图（计数模式专用；键 = 接收者类型.属性名，
// 随 get_and_clear_inline_cache_stats() 导出并清空）。
std::unordered_map<std::string, uint64_t>& icSlowSiteHistogram();

inline void incICStat(std::atomic<uint64_t>& counter) {
  if (getConfig().collect_attr_cache_stats) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
}

class LoadTypeMethodCache;

class LoadMethodCache {
 public:
  struct Entry {
    BorrowedRef<PyTypeObject> type;
    BorrowedRef<> value;
    uint32_t keys_version;
#if PY_VERSION_HEX < 0x030C0000
    // fill 记录时的 tp_version_tag，命中前拉式验证（D5，3.11 无 type
    // watcher）。
    uint32_t type_version{0};
#endif

    bool isValidKeysVersion(BorrowedRef<> obj);
  };

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 内联快路径 stub 的条目布局访问器（gen_asm 汇编发射用）。
  static constexpr size_t entriesOffset() {
    return offsetof(LoadMethodCache, entries_);
  }
  static constexpr size_t entryTypeOffset() {
    return offsetof(Entry, type);
  }
  static constexpr size_t entryValueOffset() {
    return offsetof(Entry, value);
  }
  static constexpr size_t entryKeysVersionOffset() {
    return offsetof(Entry, keys_version);
  }
  static constexpr size_t entryTypeVersionOffset() {
    return offsetof(Entry, type_version);
  }
  static constexpr size_t entrySize() {
    return sizeof(Entry);
  }
  static constexpr size_t numEntries() {
    return 4;
  }
  static constexpr size_t pressureSlotOffset() {
    return offsetof(LoadMethodCache, pressure_slot_);
  }
#endif

  ~LoadMethodCache();

  static LoadMethodResult
  lookupHelper(LoadMethodCache* cache, BorrowedRef<> obj, BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  void typeChanged(PyTypeObject* type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, BorrowedRef<> name);

  std::array<Entry, 4> entries_;
  std::unique_ptr<CacheStats> cache_stats_;

 public:
  void setPressureSlot(uint64_t* slot) {
    pressure_slot_ = slot;
  }

 private:
  uint64_t* pressure_slot_{nullptr};
#if PY_VERSION_HEX < 0x030C0000
  // 类型接收者委托缓存（劣化归因轮 C1）：通用条目按 Py_TYPE(obj)
  //（元类型）键控，慢路径又因 type_getattro ≠ GenericGetAttr 早退
  // 拒填，类型对象作接收者此前永久慢路径（docutils 的
  // type.__init__/__new__ 各 12 万次/值即此形态）。委托既有
  // LoadTypeMethodCache（type_getattro 语义复刻 + tp_version_tag
  // 拉式校验，D5）。惰性分配，零成本于不涉类型受者的站点。
  std::unique_ptr<LoadTypeMethodCache> type_recv_cache_;
#endif

#if PY_VERSION_HEX < 0x030C0000
  // 实例属性方法位（IC 计数轮：纯 Python pickle 的 self.read/readline
  // 形态——绑定方法存于实例字典，类型侧 fill 永不适用，每次调用付
  // 全量 _PyType_Lookup + 字典查找）。命中前提：类型指针 + VALID 标志
  // + tp_version_tag 拉式验证（类侧新增数据描述符等变化经版本失效），
  // 值经 me_key 自验证 hint 从实例字典/values 逐次活读（借引用不驻留，
  // 删除即自然未命中）。
  BorrowedRef<PyTypeObject> ia_type_;
  uint32_t ia_type_version_{0};
  Py_ssize_t ia_hint_{-1};
#endif
};

// A cache for LoadMethodCached instructions where we expect the receiver to be
// a type object.
//
// The first entry in `entry` is the type receiver. The second entry in `entry`
// is the cached value.
//
// The code for loading a method where the expected receiver is a type is
// specialized into a fast path and a slow path. The first element is loaded
// from the cache and compared against the receiver. If they are equal, the
// `getValueHelper()` is called which returns the cached value. If they are not
// equal, `lookupHelper()` is called, which performs the full lookup and
// potentially fills the cache.
class LoadTypeMethodCache {
 public:
  ~LoadTypeMethodCache();

  static LoadMethodResult
  lookupHelper(LoadTypeMethodCache* cache, PyTypeObject* obj, PyObject* name);

  static LoadMethodResult getValueHelper(
      LoadTypeMethodCache* cache,
      PyObject* obj);

  LoadMethodResult lookup(BorrowedRef<PyTypeObject> obj, BorrowedRef<> name);

  // Get the address of the cached type object.
  PyTypeObject** typeAddr();

  // Get the cached method value.
  BorrowedRef<> value();

  void typeChanged(BorrowedRef<PyTypeObject> type);

  void initCacheStats(const char* filename, const char* method_name);
  void clearCacheStats();
  const CacheStats* cacheStats();

 private:
  void
  fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> value, bool is_bound_meth);

  // Borrowed, but uses a raw pointer as typeAddr() will return the address of
  // this field for codegen purposes.
  PyTypeObject* type_;
  BorrowedRef<> value_;
  std::unique_ptr<CacheStats> cache_stats_;
  bool is_unbound_meth_;
#if PY_VERSION_HEX < 0x030C0000
  // fill 记录时的 tp_version_tag；3.11 不发射内联类型比较快路径，命中
  // 判定在 lookup() 内完成并以此拉式验证（D5）。
  uint32_t version_{0};
#endif
};

// A cache for an individual LoadModuleAttrCached instruction.
class LoadModuleAttrCache {
 public:
  static PyObject* lookupHelper(
      LoadModuleAttrCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  PyObject* lookup(BorrowedRef<> obj, BorrowedRef<> name);

 private:
  PyObject* lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);
  void
  fill(BorrowedRef<> obj, BorrowedRef<> value, ci_dict_version_tag_t version);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t version_{0};
  BorrowedRef<> value_;
#endif
};

class LoadModuleMethodCache {
 public:
  static LoadMethodResult lookupHelper(
      LoadModuleMethodCache* cache,
      BorrowedRef<> obj,
      BorrowedRef<> name);
  LoadMethodResult lookup(BorrowedRef<> obj, BorrowedRef<> name);
  BorrowedRef<> moduleObj();
#if PY_VERSION_HEX < 0x030E0000
  BorrowedRef<> value();
#else
  PyObject** cache() {
    return cache_;
  }
#endif

 private:
  LoadMethodResult lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name);

  // This corresponds to module __dict__'s version which allows us
  // to correctly invalidate the cache whenever the dictionary changes.
  BorrowedRef<> module_obj_;
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** cache_;
#else
  ci_dict_version_tag_t module_version_{0};
  BorrowedRef<> value_;
#endif
};

// Invalidate all load/store attr caches for type
void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type);

#if PY_VERSION_HEX < 0x030C0000
// init 期预热 slot_tp_getattr_hook 探针（C2）：消灭惰性初始化窗口，
// 避免首次触发落在异常传播中途（探针建类执行 Python 代码）。
void warmSlotTpGetattrHookProbe();
#endif

} // namespace jit

struct FunctionEntryCacheValue {
  void** ptr{nullptr};
  Ref<_PyTypedArgsInfo> arg_info;
};

using FunctionEntryCacheMap =
    jit::UnorderedMap<PyFunctionObject*, FunctionEntryCacheValue>;
