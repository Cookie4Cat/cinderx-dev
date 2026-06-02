# 详细设计说明书 — 自适应 AutoJIT 行为模式分类

## 1 产品版本&密级

| 项 | 内容 |
|---|---|
| 产品 | CinderX JIT（**目标 Python 3.14+**，含 Static Python；含 `Py_GIL_DISABLED` 自由线程构建） |
| 特性 | 自适应 AutoJIT 行为模式分类（Behavior Pattern Classification） |
| 版本 | v0.2（草案） |
| 密级 | 内部公开 |
| 运行环境 | CinderX 进程内 JIT；x86-64 与 ARM64（Kunpeng）；C++17 |

## 2 拟制信息

| 角色 | 信息 |
|---|---|
| 拟制 | CinderX 性能优化组 |
| 日期 | 2026-06-01 |
| 上游功能设计 | `docs/design/autojit-behavior-classification/【功能设计】AutoJIT 行为模式分类.md` |
| 上游需求 | `docs/design/autojit-behavior-classification/【需求分析】AutoJIT 行为模式分类.md` |

## 3 修订记录

| 版本 | 日期 | 修订人 | 修订说明 |
|---|---|---|---|
| v0.1 | 2026-06-01 | 性能优化组 | 首版。落地 5 个实现单元的数据结构、算法、行为/异常模型、内部接口与代码实现要点（C++）。 |
| v0.2 | 2026-06-02 | 性能优化组 | 根据 Phase 0 C++ dump 与 gdb 定位更新实现约束：分类 schema/evidence 可冻结，`startup_phase` 来源必须改为安全 import signal provider，禁止在 `jitVectorcall` 中遍历 frame/code metadata；policy/default 需 A/B release gate。 |

## 4 Keywords 关键词

structure_key 打包、opcode 家族表、loop_score、Phase-3 特化观测、CodeExtra 原子发布、release/acquire、jitVectorcall、computeThreshold。

## 5 Abstract 摘要

本详细设计将功能设计落到可指导编码的 C++ 实现：(1) 数据结构与编码——`StructureKey` 打包为 16-bit payload、`CodeExtra` 扩展一个 32-bit 原子发布字 `skey_word`、`opcode→工作维度` 家族表；(2) 单次字节码扫描与 `structure_key` 派生算法；(3) 特化观测与滞回带（**v1 不实现，defer 到 Phase-3，审校 T2.2**）；(4) `CodeExtra` 的 free-threaded 单字 release/acquire 发布与失败回退；(5) `jitVectorcall` 集成与**最小阈值策略** `computeThreshold`（自由函数，对明确 `raise_threshold_candidate` 抬阈值削减 compile storm，T3.1b/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11/T2.1）。2026-06-02 Phase 0 C++ clean summary 已冻结 gate-side 分类 schema/evidence 与 bootstrap 编码起点，但未冻结生产 policy/default；默认策略需 `PYTHONJITAUTO=auto[:N]` vs 数值 `N` 的 A/B 和相邻配置比较后才能发布。gdb 定位证明在 `jitVectorcall` 中遍历 Python frame/code metadata 计算 `import_stack` 会 SIGSEGV，故 `startup_phase` 必须来自安全 import signal provider，热路径只消费 provider 输出的冻结 bool；provider 通过前 startup-init 分支关闭。`high_risk` 保留为成本/安全 modifier，不再一刀切视为低 ROI；synthetic 低 ROI 默认只覆盖无 loop、非 static、ReflectionMeta/Trivial；`Mixed` 通过 `mixed_shape` 保留 top-2 维度组合；剩余并列选族采用 benefit-first tie-break；分类配置进程内冻结且缓存无运行期失效；字符串仅用于诊断解码。设计以单字原子发布消除"值/标志"排序风险，并对每个单元给出正常/异常行为模型、数据流转、内部接口定义与 DFX。

## 6 List of abbreviations 缩略语清单

| 缩略语 | 英文全称 | 中文名 |
|---|---|---|
| JIT | Just-In-Time compilation | 即时编译 |
| OSR | On-Stack Replacement | 栈上替换 |
| FT | Free-Threaded (Python) | 自由线程 Python |
| SP | Static Python | 静态 Python |
| CFG | Control Flow Graph | 控制流图 |
| ROI | Return On Investment | 收益成本比 |
| AE | Acceptance Example | 验收示例 |
| DFX | Design for X | 面向 X 的设计 |
| LSB/MSB | Least/Most Significant Bit | 最低/最高有效位 |

## 7 简介

本文档面向实现者，给出 CinderX 行为分类器的内部接口与代码实现参考，具体到 C++17 与 CinderX 运行环境。新增代码集中于 `cinderx/Jit/behavior_classifier.{h,cpp}`，并对 `cinderx/Common/code_extra.h`、`cinderx/Jit/pyjit.cpp` 做受控改动。所有跨线程字段均按 CinderX 既有原子范式实现（参照 `context.cpp` 的 `jit_compiled` release/acquire 发布）。本文沿用功能设计的术语：`structure_key`（确定性聚合身份）、`gate_context`（当次 gate 上下文，不聚合）与 `specialization_band`（Phase-3 弱旁路观测）。

---

# 8 上游文档引用

| 上游 | 关键约束（本详细设计须满足） |
|---|---|
| 需求 R18 | `structure_key` 是唯一聚合键；`gate_context` / `specialization_band` 不进键、不聚合 |
| 需求 R20 | `structure_key` 确定；band 带滞回 |
| 需求 R21/R26、KD8 | 单次 O(n) 扫描、缓存、FT release/acquire 发布、失败回退默认阈值 |
| 需求 R22/R25 | 归一遍历，每 opcode 唯一归属；band 旁路采集互不污染 |
| 功能设计 8.4–8.7 | 4 功能项的逻辑接口与调用路径 |

---

# 9 实现设计 1：数据结构与编码

## 9.1 实现概述

定义三组数据：`StructureKey` 的位打包、`CodeExtra` 的扩展字段（单字原子发布）、`opcode→WorkDim` 家族表。这是其余单元的公共底座。

## 9.2 关键算法与流程

**术语对应（审校 T4.2/T3.10）：** 上游需求/功能设计中的 `structure_key` 指**逻辑聚合身份**（解码后的 `StructureKey` 值）；本详细设计的 `skey_word` 指其**物理容器**（含 valid 位 + payload 的 32 bit 字）。凡缓存、统计、聚合契约中说"structure_key"，一律指解码值，绝不指原始字。字符串只在 Phase 0 dump、日志和诊断中由 payload 解码生成，不进入热路径、缓存或聚合主表示。数据流：`StructureKey`(值) → `pack()` → payload → `skey_word`(release 写) → acquire 读 → `unpack()` → `StructureKey`(值)。

`StructureKey` 打包为一个 ≤16 bit 的 payload，再与一个 `valid` 位组成 **单个 32 bit 发布字** `skey_word`。关键点：**用单字单次 release/acquire 发布**，从根本上消除"先写值后写标志"的排序风险（审查 Finding 3 的最稳妥实现）。

```
skey_word (uint32_t) 位布局：
  bit 31        valid           # 1=已分类（PyMem_Calloc 零初始化 => 0=未分类）
  bits [12..15] mixed_shape     # 0=none；1..15=Mixed canonical top-2 工作维度组合（T3.7）
  bits [8..11]  family          # Family 枚举，8 个值（T2.4 去 ScalarCompute；T3.4 去 ImportInit），占 4 bit
  bits [6..7]   loop_score      # 0..3
  bit  5        is_suspendable
  bit  4        is_static
  bit  3        high_risk
  bit  2        is_synthetic
  bits [0..1]   reserved (=0)
PAYLOAD_MASK = 0xFFFF ; VALID_BIT = 0x8000_0000
```

## 9.3 行为模型

### 9.3.1 正常流程

打包/解包为纯位运算，O(1)，无分配、无副作用。

### 9.3.2 异常流程

打包不产生异常。解包仅在 `valid` 位为 1 时进行（由缓存层 12 的读路径保证）。`Family` 越界不可能（由派生逻辑枚举闭集保证）；`mixed_shape!=none` 仅对 `Family::Mixed` 有意义，非 Mixed 解包时视为 `none`（或 debug 断言），避免无效组合污染聚合。

## 9.4 数据模型

### 9.4.1 数据结构定义

```cpp
// behavior_classifier.h
namespace jit {

enum class Family : uint8_t {
  // 审校 T2.4：去 ScalarCompute——compute 主导一律 NumericLoop，由 loop_score 区分有无循环
  NumericLoop = 0, BranchFSM, ObjectManipulator,
  CallDispatcher, AsyncStateMachine, ReflectionMeta,
  Trivial, Mixed,
  kCount
};
// kCount==8，须与需求 R15 的 family 枚举逐一对应（新增 family 须同步改 R15 与位宽）
static_assert(static_cast<int>(Family::kCount) == 8, "family count must match requirements R15");
static_assert(static_cast<int>(Family::kCount) <= 16, "family must fit in 4 bits");

enum class WorkDim : uint8_t {
  Compute = 0, Control, Object, Dispatch, Suspend, Dynamic, kCount,
  Neutral = 0xFF  // 不计入任何工作维度
};

// 0=非 Mixed；1..15 为 6 个 WorkDim 的 canonical unordered pair 编码（T3.7）
using MixedShape = uint8_t;
constexpr MixedShape kMixedShapeNone = 0;

// 解析后的结构身份（值类型，可平凡拷贝）
struct StructureKey {
  Family   family{Family::Trivial};
  MixedShape mixed_shape{kMixedShapeNone};
  uint8_t  loop_score{0};        // 0..3
  bool     is_suspendable{false};
  bool     is_static{false};
  bool     high_risk{false};
  bool     is_synthetic{false};

  uint16_t pack() const;                       // -> payload (无 valid 位)
  static StructureKey unpack(uint16_t payload);
};

constexpr uint32_t kSkeyValidBit = 0x80000000u;
constexpr uint32_t kSkeyPayloadMask = 0x0000FFFFu;
}
```

`CodeExtra` 扩展（`cinderx/Common/code_extra.h`，C 头文件，供解释器包含）：

```c
typedef struct CodeExtra {
  union { uint64_t calls; struct CodeExtra* next; };
  void* jit_compiled;
  void* jit_globals;
  void* jit_builtins;
  /* 新增：行为分类缓存 */
  uint32_t skey_word;   /* bit31=valid; 低位=StructureKey payload；零初始化=未分类 */
  /* uint32_t spec_band;   // Phase-3 才加（审校 T2.2：v1 不实现特化观测）*/
} CodeExtra;
```

### 9.4.2 数据流转

`scanCode`(单元 10) → `StructureKey`(值) → `pack()` → `payload | VALID_BIT` → 单字 release 发布进 `skey_word`(单元 12) → gate 读路径 acquire 加载 → `unpack()` → 供 `computeThreshold`(单元 13)。（`spec_band` 为 Phase-3 才加，不并入 `skey_word`，T2.2。）

## 9.5 接口设计

### 9.5.1 内部接口设计

公共底座接口：`StructureKey::pack/unpack`、家族表查询 `workDimOf(opcode)`。均为无副作用纯函数，供其它单元调用。

### 9.5.2 内部接口定义

```cpp
uint16_t StructureKey::pack() const;
StructureKey StructureKey::unpack(uint16_t payload);
MixedShape encodeMixedShape(WorkDim a, WorkDim b);  // canonical unordered pair；非 Mixed 使用 kMixedShapeNone
WorkDim workDimOf(int canonical_opcode);   // 家族表查询，未知→Neutral
bool isExceptionOpcode(int canonical_opcode);
bool isSpecializableOpcode(int canonical_opcode);   // Phase-3 特化观测参考，v1 不调用
```

## 9.6 代码实现要点

家族表按 canonical opcode 查表，避免每指令大 switch。**注意（审校修正）：3.14 的 Static Python opcode 不是单字节**——它们定义为 `(n | EXTENDED_OPCODE_FLAG)`，其中 `EXTENDED_OPCODE_FLAG = 0x200`（512），故 `INVOKE_METHOD=513`、`STORE_FIELD=518` 等取值 ≥512（已核实 `cinderx/Interpreter/3.14/cinder_opcode_ids.h`）。单一 256 表会把全部 SP opcode 静默归入 `Neutral`，使 SP 函数误判为 `Trivial`/`Mixed`，破坏 R24/AE3。故采用**双层表 + 掩码**：

```cpp
// behavior_classifier.cpp
namespace {
constexpr int kExtendedOpcodeFlag = 0x200;   // 与 cinder_opcode_ids.h 一致
// 基础表（0..255）覆盖 3.14 基础 opcode（目标 3.14+；旧式 3.10/3.11 opcode 在 3.14 不存在）
constexpr std::array<WorkDim, 256> buildBaseTable() {
  std::array<WorkDim, 256> t{}; for (auto& e : t) e = WorkDim::Neutral;
  auto set=[&](int op,WorkDim d){ if(op>=0&&op<256) t[op]=d; };
  set(BINARY_OP, WorkDim::Compute); set(COMPARE_OP, WorkDim::Compute); set(TO_BOOL, WorkDim::Compute);
  set(POP_JUMP_IF_FALSE, WorkDim::Control); set(FOR_ITER, WorkDim::Control);
  set(CALL, WorkDim::Dispatch); set(SEND, WorkDim::Suspend); set(LOAD_GLOBAL, WorkDim::Dynamic);
  /* …全量见 R1–R6（仅 3.14 基础 opcode） */
  return t;
}
// SP 表（按低字节索引，0..0xFF）覆盖 (n|EXTENDED_OPCODE_FLAG) 的 SP opcode
constexpr std::array<WorkDim, 256> buildSpTable() {
  std::array<WorkDim, 256> t{}; for (auto& e : t) e = WorkDim::Neutral;
  auto set=[&](int sp,WorkDim d){ int lo=sp & 0xFF; if(lo<256) t[lo]=d; };
  set(INVOKE_FUNCTION, WorkDim::Dispatch); set(INVOKE_METHOD, WorkDim::Dispatch);
  set(STORE_FIELD, WorkDim::Object); set(LOAD_FIELD, WorkDim::Object);
  set(PRIMITIVE_BINARY_OP, WorkDim::Compute); set(CAST, WorkDim::Compute);
  /* …全量 SP opcode（R24） */
  return t;
}
constexpr auto kBaseTable = buildBaseTable();
constexpr auto kSpTable   = buildSpTable();
} // namespace

WorkDim workDimOf(int canonical_op) {           // 入参为 BytecodeInstruction::opcode() 的返回值
  if (canonical_op & kExtendedOpcodeFlag) {      // SP opcode（≥512）
    return kSpTable[canonical_op & 0xFF];
  }
  return (canonical_op >= 0 && canonical_op < 256) ? kBaseTable[canonical_op] : WorkDim::Neutral;
}
```

要点：
- **入参必须是 `BytecodeInstruction::opcode()` 的返回值**（它已 unspecialize 且对 SP 复合了 `EXTENDED_OPCODE_FLAG`），不能用 `uninstrumentedOpcode()` 的原始字节（见单元 10.2.1 修正）。
- 实现时按本构建实际 opcode 枚举确认 SP opcode 集合与低字节无碰撞；如有碰撞改用 `flat_hash_map<int,WorkDim>`。
- 家族表必须有**全 opcode 覆盖单测**：断言 `builder.cpp` 处理集合中每个 opcode（含 ≥512 的 SP）都有期望归属，漏配即红（FMEA 缓解）。建议补一条**语义 golden 测试**（固定 opcode 语料的期望维度），以捕获"错配"而非仅"漏配"。

---

# 10 实现设计 2：签名扫描与 structure_key 派生

## 10.1 实现概述

对 code object 单次遍历 `BytecodeInstructionBlock`，累积 6 工作维度计数、异常子计数，并在同一遍历中就地收集后向边端点求 `loop_score`，最终派生 `StructureKey`。对应功能项 1（R1–R16、R19、R22–R25）。可特化/已特化计数 defer 到 Phase-3 特化观测。

## 10.2 关键算法与流程

### 10.2.1 单次扫描

**单次遍历同时完成两件事**：工作维度计数、**后向边端点收集**（审校修正 T1.3：既有 OSR 只暴露 `collectBackedgeTargetOffsets`（仅 target、去重、上限 16），不提供 `{source,target}`，故循环嵌套必须在本扫描内就地记录 `source`+`getJumpTarget()`，既消除不存在的依赖，又让"单次 O(n) 扫描"名副其实）。v1 不做特化旁路采集（审校 T2.2，特化观测 defer 到 Phase-3）。

```cpp
struct Signature {
  std::array<uint32_t, (size_t)WorkDim::kCount> counts{};
  uint32_t exc{0};            // 异常子计数（供 high_risk 派生）
  uint32_t n_eff{0};          // 有效指令（分母）
  // 后向边端点 {source_idx, target_idx}，就地收集，固定上限避免堆分配
  llvm::SmallVector<std::pair<int,int>, 16> backedges{};
  uint8_t  loop_score{0};
  // Phase-3 才加：specializable / specialized（特化观测分母/分子，单元 11）
};

Signature scanCode(BorrowedRef<PyCodeObject> code) {
  Signature s;
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    int op = instr.opcode();          // 公有：已 unspecialize 且对 SP 复合 EXTENDED_OPCODE_FLAG（R22）
    if (op == EXTENDED_ARG) continue; // 非语义跳过（迭代器已处理 EXTENDED_OPCODE）
    // —— 工作维度归类（R22/R25 唯一归属）——
    WorkDim d = workDimOf(op);
    if (d != WorkDim::Neutral) s.counts[(size_t)d]++;
    if (isExceptionOpcode(op)) s.exc++;
    // —— 后向边就地收集（T1.3）——
    if (isBackwardJump(op)) {                       // JUMP_BACKWARD / JUMP_BACKWARD_NO_INTERRUPT / *_JIT / *_NO_JIT
      BCIndex src = instr.opcodeIndex();
      BCIndex tgt = instr.getJumpTarget().asIndex(); // 与 src 同为 instruction index 坐标
      if (tgt < src && s.backedges.size() < 16) {
        s.backedges.push_back({src.value(), tgt.value()});
      }
    }
    s.n_eff++;
  }
  s.loop_score = loopScore(s.backedges);             // 见 10.2.2
  return s;
  // Phase-3：在归类后加一段 isSpecializableOpcode(op) ? specializedOpcode()!=op 的旁路计数
}
```

### 10.2.2 loop_score（嵌套深度，就地端点）

用扫描中就地收集的后向边端点（10.2.1），将每条边视为区间 `[target_idx, source_idx]`。bootstrap `loop_score`（T3.9）取嵌套深度和多 backedge 数两路的最大值：`nesting_score = min(max_static_nesting_depth, 3)`；`count_score = 0/1/2/3` 对应 backedge 数 `0 / 1 / 2–3 / >=4`。**无第二次扫描、无堆分配**（边集为栈上 `SmallVector`，上限 16）：

```cpp
uint8_t loopScore(const llvm::SmallVectorImpl<std::pair<int,int>>& edges) {
  if (edges.empty()) return 0;
  // 事件法：+1 在 target，-1 在 source 之后；求最大同时覆盖数
  llvm::SmallVector<std::pair<int,int>, 32> ev;      // (idx, delta)，栈上
  for (auto& [src, tgt] : edges) { ev.push_back({tgt,+1}); ev.push_back({src+1,-1}); }
  std::sort(ev.begin(), ev.end());
  int cur = 0, depth = 0;
  for (auto& [idx, d] : ev) { cur += d; depth = std::max(depth, cur); }
  uint8_t nesting_score = (uint8_t)std::min(depth, 3);
  uint8_t count_score =
      edges.size() >= 4 ? 3 : (edges.size() >= 2 ? 2 : 1);
  return std::max(nesting_score, count_score);
}
```

> 注：既有 `collectBackedgeTargetOffsets`（`cinderx/Jit/osr.cpp:327`）的 16 条上限语义在此保留（`s.backedges.size() < 16`），深嵌套（>16 边）截顶到 `loop_score=3`，对 0–3 分级无影响。

### 10.2.3 派生（预过滤 → 分桶 → 选族）

```cpp
StructureKey deriveStructureKey(BorrowedRef<PyCodeObject> code) {
  StructureKey k;
  // 结构修饰位（与族正交）
  k.is_static      = code->co_flags & CI_CO_STATICALLY_COMPILED;     // R9
  k.is_suspendable = code->co_flags & (CO_GENERATOR|CO_COROUTINE|CO_ASYNC_GENERATOR); // R10

  // R12/T3.4：缺 OPTIMIZED|NEWLOCALS 或 <module> 的初始化代码只进 Phase 0
  // InitCodeDiagnostic 诊断桶；v1 gate 热路径不应把它编码进 StructureKey。
  JIT_DCHECK(hasRequiredFlags(code) && !isModuleName(code), "non-gate init code is diagnostic-only");
  Signature s = scanCode(code);
  k.loop_score = s.loop_score;                                        // R7
  k.high_risk  = deriveRisk(s, code);                                 // R8（见 10.2.4）
  k.is_synthetic = isSyntheticFilename(code);                         // T3.6：低 ROI 与风险分离

  if (allBelowFloor(s)) { k.family = Family::Trivial; return k; }     // R13

  auto buckets = bucketize(s);                                        // R14/T3.9（bootstrap density+floor；热路径前冻结）
  auto [d1, d2] = topTwo(buckets, kBenefitFirstTieBreakOrder);          // T3.8：主导 / 次主导；并列按收益优先
  if (bucket(buckets,d1) - bucket(buckets,d2) <= kMixedBucketDelta
      && bucket(buckets,d2) >= kMixedMinBucket) {                     // R16/T3.7/T3.9
    k.family = Family::Mixed;
    k.mixed_shape = encodeMixedShape(d1, d2);                          // T3.7：保留 Mixed top-2 组合
    return k;
  }
  k.family = mapDominant(d1, k.loop_score);                           // R15 映射表；风险由 modifier/policy 处理
  k.mixed_shape = kMixedShapeNone;
  return k;
}
```

### 10.2.4 risk 派生（不重复计 opcode，R8/R25）

```cpp
bool deriveRisk(const Signature& s, BorrowedRef<PyCodeObject> code) {
  bool async_sm   = bucketOf(s, WorkDim::Suspend) >= 2;
  bool heavy_dyn  = bucketOf(s, WorkDim::Dynamic) >= 2;
  bool exc_heavy  = s.n_eff && (s.exc * 1.0 / s.n_eff) > kExcRatio;   // 读已统计的 exc，不重扫
  bool huge       = codeLen(code) > kHugeCodeLen;
  return async_sm || heavy_dyn || exc_heavy || huge;                  // synthetic 独立进 is_synthetic(T3.6)
}
```

## 10.3 行为模型

### 10.3.1 正常流程

输入合法 code object（已过功能项 4 的 `hasRequiredFlags` 前置/或预过滤命中）→ 单次遍历 → 返回闭集中的唯一 `Family` + 修饰位。

### 10.3.2 异常流程

- code 无指令（空 block）：`n_eff=0` → `allBelowFloor` 真 → `Trivial`（不除零，分桶对 `n_eff==0` 短路）。
- backedge 收集返回空：`loop_score=0`，正常。
- 全 Neutral（无工作维度）：`Trivial`。
- 不抛 C++ 异常：纯计算路径，便于在 gate 上无 try/catch 调用。

## 10.4 数据模型

### 10.4.1 数据结构定义

见单元 9（`StructureKey`、`Signature`）。`bucketize` 产出固定长度 `std::array<uint8_t,6>`（每维 0..3）。

### 10.4.2 数据流转

`PyCodeObject*`(只读) → `Signature`(栈上) → `StructureKey`(值返回)。`Signature` 不逃逸、不堆分配（计数器为栈数组；`loopScore` 内部 `vector` 规模 = 2×backedge 数，通常极小）。

## 10.5 接口设计

### 10.5.1 内部接口设计

`deriveStructureKey` 为单元 12（缓存）唯一调用；其余为文件内静态辅助。无副作用（特化计数只读、不改 code）。

### 10.5.2 内部接口定义

```cpp
StructureKey deriveStructureKey(BorrowedRef<PyCodeObject> code);   // 纯函数
// 文件内静态：scanCode / loopScore / bucketize / topTwo / mapDominant / deriveRisk
```

## 10.6 代码实现要点

- 遍历用既有 `BytecodeInstructionBlock` 迭代器（`bytecode.h:111/163`），不手算偏移。
- **opcode 取值必须用公有 `BytecodeInstruction::opcode()`**（`bytecode.cpp:106`，已 unspecialize 且对 SP 复合 `EXTENDED_OPCODE_FLAG`）——**不要**用 `private` 的 `uninstrumentedOpcode()`（编译不可见，且返回未复合 flag 的 ≤255 原始字节，会漏掉全部 SP opcode，审校 T1.2）。（Phase-3 才加特化判定 `specializedOpcode() != opcode()`，与工作维度同遍历、互不污染，R22；v1 不做，T2.2。）
- 后向边在同一遍历内就地收集（10.2.1），不调用第二遍 `collectBackedgeTargetOffsets`，使"单次 O(n) 扫描"成立（审校 T1.3）。
- `bucketize` 必须对 `n_eff==0` 与低于 floor 短路，杜绝除零与微函数假信号（R14）。
- bootstrap 常量集中为可调常量：`kDimCountFloor=2`、density cutoff `0.10/0.25/0.50`、`kMixedBucketDelta=1`、`kMixedMinBucket=2`、loop count score 阈值 `1/2/4`。这些常量已由 2026-06-02 Phase 0 C++ gate-side dump 通过 Mixed/family 红线，可作为 v1 编码起点；它们不是生产 policy/default 冻结结论。正式默认值需经 `auto[:N]` vs 数值 `N` A/B 和相邻 cutoff/floor/δ/loop 配置比较后冻结；后续重新标定必须在新进程或清空 code objects 后进入 gate/cache/policy，并保留部署覆盖入口（T3.3/T3.9）。

---

# 11 Phase-3 参考设计：特化观测与滞回（v1 不实现）

> **⚠ v1 不实现（审校 T2.2）：** T3.1(b) 最小策略不读 `specialization_band`，本单元整条（`spec_band` 字段、`readSpecializationBand`、`applyHysteresis`、AE10）**defer 到 Phase-3**，与计数式真稳定性信号一起做。下文为 Phase-3 设计参考；v1 不创建 `CodeExtra.spec_band`、不在扫描内做特化旁路采集（单元 10 的 `s.specializable/s.specialized` 在 v1 可不计）。

## 11.1 实现概述

把单元 10 采集到的 `specialized/specializable` 计数转为带滞回的 `SpecBand`，缓存于 `CodeExtra.spec_band`，gate 时读取。弱信号，不进 `structure_key`（R11、KD6、R20）。

## 11.2 关键算法与流程

```cpp
SpecBand readSpecializationBand(BorrowedRef<PyCodeObject> code, const Signature* fresh) {
  CodeExtra* ex = codeExtra(code);
  SpecBand prev = ex ? (SpecBand)_Py_atomic_load_uint32_relaxed(&ex->spec_band) : SpecBand::Low;
  double presence = (fresh && fresh->specializable)
                  ? (double)fresh->specialized / fresh->specializable : 0.0;       // R11
  SpecBand band = applyHysteresis(presence, prev);                                  // R20
  if (ex) _Py_atomic_store_uint32_relaxed(&ex->spec_band, (uint32_t)band);
  return band;
}

// 进/出高带不同 cutoff，避免边界抖动
SpecBand applyHysteresis(double p, SpecBand prev) {
  // 上行阈：Low->Mid=U1, Mid->High=U2；下行阈：High->Mid=D2, Mid->Low=D1；U2>D2, U1>D1
  switch (prev) {
    case SpecBand::Low:  return p >= kU1 ? (p >= kU2 ? SpecBand::High : SpecBand::Mid) : SpecBand::Low;
    case SpecBand::Mid:  return p >= kU2 ? SpecBand::High : (p < kD1 ? SpecBand::Low : SpecBand::Mid);
    case SpecBand::High: return p < kD2  ? (p < kD1 ? SpecBand::Low : SpecBand::Mid) : SpecBand::High;
  }
  return SpecBand::Low;
}
```

`presence` 重读频率属于 Phase-3 待定项：可选每 gate 重算，或缓存 + 每 N 次惰性刷新。v1 不实现 `CodeExtra.spec_band`，不做首扫缓存，也不为此增加热路径刷新逻辑。

## 11.3 行为模型

### 11.3.1 正常流程

`presence` 在 cutoff 之间稳定时，band 不变；跨越上/下阈才迁移，且上/下不对称（滞回）。

### 11.3.2 异常流程

- `specializable==0`（无可特化 opcode）：`presence=0` → 收敛到 `Low`，正常。
- `codeExtra==NULL`：返回上次值不可得，按 `Low` 处理；不影响 `structure_key`（band 仅微调）。

## 11.4 数据模型

### 11.4.1 数据结构定义

`CodeExtra.spec_band`（uint32，relaxed 原子）只属于 Phase-3 参考设计；v1 不在 `CodeExtra` 增加该字段。`SpecBand` 可在 Phase-3 模块局部定义为 `enum class SpecBand : uint8_t { Low, Mid, High }`。

### 11.4.2 数据流转

`Signature.{specialized,specializable}`(单元 10) → `presence` → `applyHysteresis(prev)` → `SpecBand` →（仅）单元 13 的 `threshold` 微调。**绝不进入任何聚合键**（R18，代码评审守门）。

## 11.5 接口设计

### 11.5.1 内部接口设计

`readSpecializationBand` 仅被单元 13 的便捷封装调用。输出类型 `SpecBand` 在静态分析上与 `StructureKey` 隔离（不同类型，禁止参与 map key 构造）。

### 11.5.2 内部接口定义

```cpp
SpecBand readSpecializationBand(BorrowedRef<PyCodeObject> code, const Signature* fresh);
// 约束（注释+评审）：SpecBand 不得出现在统计/反馈的聚合键中
```

## 11.6 代码实现要点

- band 用 relaxed 原子即可（仅提示，不参与正确性）。
- 滞回阈 `kU1<kU2`、`kD1<kD2` 且 `U>D`，集中为可调常量，标注"待标定 + 滞回宽度"。
- 多态回归测试（AE10）必须存在：单态预热→交替形态调用，断言 band 高时不被单独用于"提前编译"判定。

---

# 12 实现设计 4：structure_key 缓存与 free-threaded 发布

## 12.1 实现概述

把 `StructureKey` 以**单字 release/acquire** 发布进 `CodeExtra.skey_word`，命中即 O(1)，未命中扫描一次；并发首次良性；缓存不可用回退默认阈值。分类 schema/config 进入 gate/cache/policy 前冻结；命中后不失效、不做 schema/version 比对。对应功能项 3（R21、R26、KD8/T3.11）。

## 12.2 关键算法与流程

```cpp
// 返回 std::optional：nullopt 表示缓存不可用 → 调用方回退默认阈值（KD8(c)）
std::optional<StructureKey> getOrComputeStructureKey(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* ex) {
  if (ex == nullptr) return std::nullopt;          // 分配/shutdown 失败 → 回退

  uint32_t w = _Py_atomic_load_uint32_acquire(&ex->skey_word);   // acquire
  if (w & kSkeyValidBit) {                                         // 命中
    // T3.11：分类配置进程内冻结，valid 后无需 schema/version 比对或重算。
    return StructureKey::unpack((uint16_t)(w & kSkeyPayloadMask));
  }
  // 未命中：计算（纯函数）。FT 下多线程可并发到此，结果逐位相等 → 良性。
  StructureKey k = deriveStructureKey(code);
  uint32_t payload = k.pack() | kSkeyValidBit;
  _Py_atomic_store_uint32_release(&ex->skey_word, payload);       // 单字 release 发布
  return k;
}
// Phase-3：特化观测恢复时，可加 Signature* out_sig 回填以复用同一次扫描（省单元 11 重扫）
```

要点：**单字发布** = 不存在"值已写、标志未写"的中间态；读侧一次 acquire 取整字，valid 位与 payload 同源，无撕裂、无半初始化（解决审查 Finding 3 的根因）。GIL 构建下 `_Py_atomic_*` 退化为普通访问。

> 术语对应（审校 T4.5）：需求 R26 / AE11 所称的"initialized 标志"，在此实现为 `skey_word` 的 **bit31（valid 位）**——把"值"与"标志"合并进单字、单次 acquire 读取，正是避免半初始化的手段；不存在独立的 `classified` 字段。

## 12.3 行为模型

### 12.3.1 正常流程

首次：未命中 → 扫描 → release 发布。后续：acquire 命中 → 解包返回。`skey_word` 一旦 valid，在当前进程内不失效；分类配置变化只允许发生在 Phase 0 dump 或新进程/清空 code objects 之后。

### 12.3.2 异常流程

- `codeExtra==NULL`（OOM、`SetExtra` 失败、shutdown，见 `code.cpp:185`）：返回 `nullopt` → gate 回退 `config.compile_after_n_calls`，不崩、不读半初始化（KD8）。
- FT 并发首次：两线程都算并都 release 写；因纯函数结果相同，最后写者胜出无害；亦可选 `compare_exchange` 只发布一次（二选一，见 12.6）。
- 运行期分类配置变更：v1 不支持。配置已冻结且 `skey_word` 不含 schema/version 字段；如需变更 cutoff/floor/δ/tie-break/payload 布局，必须新进程或清空 code objects。

## 12.4 数据模型

### 12.4.1 数据结构定义

`CodeExtra.skey_word`（uint32，bit31=valid）。新增原子访问内联（置于 `code_extra.h`，与既有 `Ci_code_extra_*` 同风格）：

```c
#ifdef Py_GIL_DISABLED
static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* e) {
  return _Py_atomic_load_uint32_acquire(&e->skey_word);
}
static inline void Ci_code_extra_store_skey_release(CodeExtra* e, uint32_t w) {
  _Py_atomic_store_uint32_release(&e->skey_word, w);
}
#else
static inline uint32_t Ci_code_extra_load_skey_acquire(const CodeExtra* e){ return e->skey_word; }
static inline void Ci_code_extra_store_skey_release(CodeExtra* e, uint32_t w){ e->skey_word = w; }
#endif
```

### 12.4.2 数据流转

`StructureKey`(单元 10) → `pack()|VALID` → `skey_word`(release) →〔下次〕acquire → `unpack()` → 单元 13。

## 12.5 接口设计

### 12.5.1 内部接口设计

热路径入口为 `getOrComputeStructureKey(code, ex)`：`ex` 由单元 13 的 gate helper 统一取得并同时服务 calls 与 `structure_key`，避免在准入路径做两次 `codeExtra(code)` get-or-create。非热路径诊断 dump 可提供便捷包装自行获取 `CodeExtra`。内部调用单元 10 的 `deriveStructureKey`。（Phase-3 恢复特化观测时可加 `Signature* out_sig` 回填以复用同一次扫描。）

### 12.5.2 内部接口定义

```cpp
std::optional<StructureKey> getOrComputeStructureKey(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* ex);
// code_extra.h: Ci_code_extra_load_skey_acquire / _store_skey_release
```

## 12.6 代码实现要点

- 发布方案二选一并在实现注释说明：**(默认) 良性重复 + 最后写者胜**（最简，纯函数保证一致）；或 **`compare_exchange_strong`** 只发布一次（省重复计算，代价是一次 CAS）。鉴于扫描廉价且重复概率低，默认取前者。
- `CodeExtra` 新增字段（v1 仅 `skey_word`）置于 `jit_builtins` 之后，避免改动 `union calls/next` 与既有偏移敏感代码；`PyMem_Calloc` 已零初始化 → `skey_word=0` 即未分类。（`spec_band` Phase-3 才加；v1 不加 schema/version 位，T3.11。）
- 实现时确认目标构建可用的 32 位 acquire/release 原子 helper；若 `_Py_atomic_load_uint32_acquire/_store_uint32_release` 不存在，使用等价 32 位 acquire/release helper 或封装本特性专用 helper。

---

# 13 实现设计 5：jitVectorcall 集成与最小阈值策略 computeThreshold

> **审校决策与 Phase 0 C++ 证据已回灌：** T3.1(b)/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11 默认策略不再是 no-op，而是对明确 `raise_threshold_candidate` 抬阈值削减 compile storm；不可达 module/class body 只进 Phase 0 `InitCodeDiagnostic`；Phase 0 C++ clean summary 已通过 Mixed/family 红线，可冻结分类 schema/evidence 与编码起点，但不能冻结生产 policy/default；`startup_phase` 来源仍未冻结，且 gdb 证明 `jitVectorcall` 内 frame/code metadata import-stack 采样不安全；`high_risk` 不再一刀切等同低 ROI；synthetic 低 ROI 默认只覆盖无 loop、非 static、ReflectionMeta/Trivial；`Mixed` 记录 top-2 `mixed_shape`；剩余并列选族采用 benefit-first tie-break；`structure_key` 物理缓存固定为 32-bit `skey_word`，字符串仅作诊断展示；分类配置进程内冻结，`skey_word` 无运行期失效；T2.1 `AutoJitPolicy` 虚类降为自由函数 `computeThreshold`；T2.2 v1 不读特化观测；T2.3 不新增环境变量，复用 `PYTHONJITAUTO=auto[:N]` 启用。

## 13.1 实现概述

在 `jitVectorcall`（`cinderx/Jit/pyjit.cpp:183`，阈值门 `:197`）注入一次分类 + 阈值计算；提供**最小有用策略**（对 low_roi / startup-init / risk-defer candidate 抬阈值，其余族走现状），使 v1 即有可测收益。**启用复用 `PYTHONJITAUTO`（不新增 env，T2.3）**：`=auto[:N]` 开分类、`=<N>` 回到现状固定阈值。对应功能项 4（R18、R20、R26、KD2/KD8、T3.1b/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11）。其中 `startup_init_candidate` 只有在安全 import signal provider 已冻结时才可启用；provider 缺失时不得用 `module_initializing` 或 `early_window` 单独替代完整 ImportInit。

## 13.2 关键算法与流程

**改造点（最小侵入）**：把原"取全局 `compile_after_n_calls`"替换为"分类 → `computeThreshold`（失败/开关关 回退全局）"。

```cpp
// 热路径只消费安全 import signal provider 输出的冻结 bool；
// Phase 0 dump 另行输出 StartupSignalMask 诊断字段。
struct GateContext {
  bool startup_phase;
};

struct AutoJitGateState {
  CodeExtra* extra;
  uint64_t calls;
  GateContext context;
};

AutoJitGateState readAutoJitGateState(BorrowedRef<PyCodeObject> code) {
  CodeExtra* ex = codeExtra(code);
  uint64_t calls = ex != nullptr ? Ci_code_extra_get_calls(ex) : 0;
  return {ex, calls, readGateContext()}; // startup_phase 读取安全 provider；不入 StructureKey
}

// pyjit.cpp jitVectorcall —— 改造后核心片段
BorrowedRef<PyCodeObject> code{func->func_code};
AutoJitGateState state = readAutoJitGateState(code);           // 内部只做一次 codeExtra get
CodeExtra* ex = state.extra;
uint64_t calls = state.calls;
uint32_t global = getConfig().compile_after_n_calls.value_or(kNoAutoJit);

uint32_t limit;
if (!getConfig().auto_classify) {                        // PYTHONJITAUTO=数值 → 分类关，等价现状（T2.3）
  limit = global;
} else if (auto sk = getOrComputeStructureKey(code, ex); !sk.has_value()) {
  limit = global;                                        // 缓存不可用 → 回退（KD8/R26）
} else {
  limit = computeThreshold(*sk, state.context, global);  // 单元 13 最小策略（无 SpecBand，T2.2）
}

if (calls < limit) {
  return getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);  // 解释（现状）
}
return forcedJitVectorcall(func_obj, stack, nargsf, kwnames);                // 编译（现状）
```

**最小策略（T3.1b，自由函数 T2.1）**：

```cpp
// 只对明确 defer candidate 抬高阈值（晚编译/几乎不编译），削减启动期 compile storm；
// 其余族走现状全局阈值。倍数为可 env 覆盖的保守默认（T3.3）。
uint32_t computeThreshold(const StructureKey& sk, const GateContext& ctx, uint32_t global) {
  bool startup_init_candidate =
      ctx.startup_phase &&
      !sk.is_static &&
      sk.loop_score == 0 &&
      (sk.family == Family::CallDispatcher ||
       sk.family == Family::ReflectionMeta ||
       sk.family == Family::ObjectManipulator ||
       sk.family == Family::BranchFSM);
  bool synthetic_low_roi_candidate =
      sk.is_synthetic &&
      sk.loop_score == 0 &&
      !sk.is_static &&
      (sk.family == Family::ReflectionMeta || sk.family == Family::Trivial);
  bool low_roi_candidate =
      sk.family == Family::Trivial || synthetic_low_roi_candidate; // synthetic 高 loop/static 不默认 defer(T3.6)
  bool compile_risk_defer_candidate =
      sk.high_risk && sk.loop_score == 0 && !sk.is_static;   // high_risk 不是低 ROI 的同义词(T3.6)
  bool raise_threshold_candidate =
      low_roi_candidate || startup_init_candidate || compile_risk_defer_candidate;
  if (raise_threshold_candidate) {
    return saturating_mul(global, kDeferThresholdFactor);    // 默认 ×N，env 可覆盖
  }
  return global;
}
// 出现第二种策略时再提升为多态接口（YAGNI，T2.1）
```

## 13.3 行为模型

### 13.3.1 正常流程

分类成功且开关开 → `computeThreshold` 给阈值（low_roi / startup-init / risk-defer candidate 抬高，其余=全局）→ 解释/编译二分不变。

### 13.3.2 异常流程

- `sk==nullopt`（缓存不可用）→ 回退全局阈值。
- 分类关（`PYTHONJITAUTO=<N>` 数值）→ 不取分类、直接用全局阈值，等价现状（A/B 对照/止血，T2.3）。
- `computeThreshold` 为纯函数、不抛异常进入 gate。

## 13.4 数据模型

### 13.4.1 数据结构定义

`computeThreshold` 为自由函数（T2.1，无策略对象/单例）；新增配置项 `config.auto_classify`（bool，由 `PYTHONJITAUTO=auto[:N]` 解析置真，T2.3）；`GateContext` 为当次 gate 上下文（v1 热路径仅 `startup_phase`，来源为安全 import signal provider，不入 `structure_key` / 不聚合）；`StartupSignalMask` 只属于 Phase 0 诊断 dump，用于比较 importlib/module initializing、安全 import 状态 provider、早期进程窗口等候选信号，早期进程窗口不得单独成为默认策略来源；`kDeferThresholdFactor` 为抬阈值倍数（可 env 覆盖，T3.3）；`kNoAutoJit` 表"AutoJIT 未启用"的哨兵。

**Import signal provider 约束：**
- 禁止在 `jitVectorcall` 中遍历 Python frame stack 或读取上层 frame code metadata 来判断 import stack；该路径已由 gdb 定位为 SIGSEGV。
- provider 应在 import machinery C 入口或其它安全点维护轻量状态，例如 thread-local import depth/counter、模块初始化安全标志或等价信号；候选挂点包括 `_PyEval_ImportName` / `_PyEval_LazyImportName` 及 `_PyEval_ImportFrom` / `_PyEval_LazyImportFrom` 对应入口，实际以目标版本生成代码和 import machinery 链路为准。`readGateContext()` 只做 O(1) 读取。
- `module_initializing` 可作为辅助诊断信号，但 clean summary 中只覆盖 795/30605 个 storm，不能单独冻结为 `startup_phase`。
- `early_window` 可作为辅助/兜底候选，但不得单独成为默认策略来源。
- provider 缺失或未通过 Phase 0.5 复跑时，`startup_phase=false` 且 startup-init 策略分支关闭；low_roi / risk-defer 分支可分阶段验证，但 v1 release 或 ImportInit 收益声明必须等待 provider 通过。

### 13.4.2 数据流转

`StructureKey(family + mixed_shape + modifiers) + GateContext` → `computeThreshold` → `limit` → 与 `calls` 比较。聚合统计（下游）按完整 `StructureKey` 落库（R18/T3.7）；`GateContext` 只影响当次阈值，不落库、不聚合。Phase 0 可输出 `StartupSignalMask` 诊断字段，但它不进入 `skey_word` 或聚合 key；接入热路径前必须把安全 import signal provider 输出折叠为冻结后的 `startup_phase` bool。**v1 无 `SpecBand`（T2.2）。**

> 术语对应（审校 T4.7/T3.4）：需求 R18 的 `gate_view` 是**概念名**；v1 中它等于 `structure_key + gate_context`（特化观测 defer 后不含 `SpecBand`），不引入持久化 `gate_view` 结构体。

## 13.5 接口设计

### 13.5.1 内部接口设计

`jitVectorcall` 只依赖 `readAutoJitGateState` + `getOrComputeStructureKey` + `computeThreshold`。`computeThreshold` 是唯一阈值决策点；下游升级策略时替换其实现（或在此提升为接口），不触碰分类器与 gate。

### 13.5.2 内部接口定义

```cpp
// startup_phase 是安全 import signal provider 冻结后的热路径输入；
// 候选信号 mask 只在诊断 dump 中出现。
struct GateContext {
  bool startup_phase;
};
struct AutoJitGateState {
  CodeExtra* extra;
  uint64_t calls;
  GateContext context;
};
uint32_t computeThreshold(const StructureKey& sk, const GateContext& ctx, uint32_t global);  // 自由函数（T2.1）
AutoJitGateState readAutoJitGateState(BorrowedRef<PyCodeObject> code);
// readGateContext() 只能 O(1) 读取 provider 状态；不得遍历 Python frame stack。
// 启用由 PYTHONJITAUTO 解析决定（不新增 env，T2.3）：
//   -X jit-auto(空 X-option) -> 现状阈值 1, auto_classify=false
//   N        -> config.compile_after_n_calls=N, config.auto_classify=false  （现状）
//   auto     -> auto_classify=true, 全局阈值取默认
//   auto:N   -> auto_classify=true, 全局阈值=N
// 聚合契约（注释强约束）：任何 pattern 统计 key == StructureKey（v1 无 SpecBand）
```

## 13.6 代码实现要点

- 改造仅限 `jitVectorcall` 内"求 limit"一段；解释/编译两条返回路径完全沿用现状（`getInterpretedVectorcall`/`forcedJitVectorcall`），降低回归面。
- **复用 `PYTHONJITAUTO`（不新增 env，T2.3）**：把其注册从 `void(uint32_t)` 改为 `void(const std::string&)`（FlagProcessor 已有该重载，`jit_flag_processor.h:84`），解析 `auto[:N]` 设 `config.auto_classify` + base 阈值，数值仍走原路径。`=N`（数值）= 分类关、等价现状，即 A/B 对照/止血手段。parser contract：

| 输入 | 解析结果 | 说明 |
|---|---|---|
| `-X jit-auto`（空 X-option） | `compile_after_n_calls=1`，`auto_classify=false` | 保留现状；FlagProcessor 只把空 X-option 视为 1，空 env 不等价于 1 |
| 十进制 `uint32_t N` | `compile_after_n_calls=N`，`auto_classify=false` | 数值路径逐函数等价现状 |
| `auto` | `compile_after_n_calls=默认值`，`auto_classify=true` | 开启分类 + 最小策略 |
| `auto:N` | `compile_after_n_calls=N`，`auto_classify=true` | 开启分类，base=N |
| malformed / negative / empty env / overflow | 记录 invalid，字段保持原值 | 不把错误输入静默转成自动分类或阈值 1 |

- **回归基线**：开关关时编译函数集合与现状逐函数 bit-for-bit 一致（CI 守门）；开关开时仅 `raise_threshold_candidate` 编译时机后移，可单独验证收益。
- `kDeferThresholdFactor`、bucket cutoff/floor、Mixed δ、loop count score 等常量集中为可 env 覆盖的 bootstrap defaults。正式热路径默认值按 T3.2/T3.3/T3.9/T3.11 标定协议（混合语料）冻结：必须比较 `PYTHONJITAUTO=auto[:N]` 与数值 `N`，并至少比较一组相邻 cutoff/floor/δ/loop 设置；冻结后进程内不可变。
- `readGateContext()` 实现必须有 gdb/smoke 验证：import-time JIT smoke 在 gdb 下正常退出；不得复现 `unicodeAsStringNoError -> isImportFrame -> hasImportStack -> recordGate -> jitVectorcall` crash 链。

---

# 14 DFX分析

## 14.1 可靠性分析

| 风险 | 设计对策 | 验证 |
|---|---|---|
| 半初始化/撕裂读 `structure_key` | **单字** release/acquire 发布（单元 12.2），无值/标志分离 | AE11 + TSan |
| 统计被切碎（Phase-3 band 入键） | 类型隔离 + 聚合契约 + 评审守门（单元 11.5/13.5） | AE8 + 代码评审 |
| Mixed 聚合过粗 | `mixed_shape` 记录 canonical top-2 工作维度组合，最多 15 种 | AE12 + Phase 0 分布 |
| import-stack 采样崩溃 | 禁止热路径 frame stack/code metadata 遍历；改用安全 import signal provider | gdb smoke 必须正常退出 |
| 新 opcode 漏归类 | 家族表全 opcode 覆盖单测 | 单元 9.6 单测 |
| 误把"曾单态"当稳定（Phase-3） | band 弱语义 + 限幅 + 滞回 | AE10 |
| 分类器异常拖垮 gate | 纯计算无抛异常路径；失败→回退默认阈值 | AE11 + 故障注入 |

## 14.2 异常处理设计

- **缓存不可用**：gate helper 取得的 `CodeExtra*` 为 `nullptr` 或 `getOrComputeStructureKey(code, ex)` 返回 `nullopt` 时，gate 回退 `config.compile_after_n_calls`（KD8(c)）。不崩、不读半态。
- **空/退化 code**：`scanCode` 对 `n_eff==0` 短路 → `Trivial`；`bucketize` 防除零。
- **backedge 为空**：`loop_score=0`。
- **开关关闭**：全路径等价现状，作为兜底回退手段。
- **import signal provider 不可用**：`startup_phase=false` 且关闭 startup-init 分支，不得退而使用 `early_window` 单独判定 ImportInit。
- 全链路无 C++ 异常向 `jitVectorcall` 传播（gate 不设 try/catch）。

## 14.3 性能分析

- 命中路径：一次 `codeExtra` 取 + 一次 acquire 读 + 一次策略调用（默认 O(1)）。相对现状 `countCalls` 已做的 `codeExtra` 取，仅多一次 acquire-load + 一次（默认内联）策略调用。
- 首次路径：**单次** O(n) 扫描（n=指令数，工作维度 + 后向边一遍完成，T1.3）+ 一次 release 发布。**不再有第二遍 backedge 扫描。** 特化计数随 Phase-3 恢复。
- FT 良性重复：最多 O(线程数) 次首扫，概率低、每次 O(n)，可接受；如实测偏高改 `compare_exchange` 单发布。
- **验收标准（R21，审校 T4.6 补全）：**
  - (V1) 稳态命中路径：准入路径单次开销相对基线回归 **≤ 2%**（startup micro-bench，以 `compile_after_n_calls` 现状为基线）。
  - (V2) 启动期：compile-storm 场景（数千 code object 同期跨阈）首扫总耗时相对"被推迟的一次编译"占比 **< 5%**（实测，超标则评估 `compare_exchange` 或惰性分类）。
  - (V3) 等价性：`PYTHONJITAUTO=<N>` / `auto_classify=false` 下，编译函数集合与调用计数与现状**逐函数 bit-for-bit 一致**（CI 回归对比，作为分类/缓存基建零行为变更的硬门）；`PYTHONJITAUTO=auto[:N]` 下仅 `raise_threshold_candidate` 按 `computeThreshold` 后移。
  - (V4) Import signal 安全性：包含 import-time JIT smoke 的 gdb 运行必须正常退出；`startup_phase` provider 复跑 Phase 0.5 dump 后，需报告 startup signal 覆盖率/误伤率，且不得只以 `module_initializing` 或 `early_window` 单信号冻结。
  - (V5) 策略/default A/B：`PYTHONJITAUTO=auto[:N]` 相对 `PYTHONJITAUTO=N` 在 pyperformance + import/dispatch 密集真实 workload 上必须减少 `raise_threshold_candidate` 编译次数和编译总耗时；非 candidate 的编译行为保持等价，启动/吞吐无显著回归。默认冻结前至少比较一组相邻 cutoff/floor/δ/loop 配置。
  - (V6) ROI 守门：synthetic 高 loop/static/generated 与 risk-defer candidate 需用 top call-count 样本证明 saved compile cost 大于 lost execution acceleration；否则默认禁用或按 family/code size 收窄。

## 14.4 安全和韧性分析

- 攻击面：仅只读 code object 既有字节码与 flags，无外部输入解析、无新增 syscalls。
- 内存所有权：复用 CPython code-extra 机制（`PyMem_Calloc`/`PyMem_Free`），生命周期随 code object，无新增泄漏路径。
- 韧性：任意子部件失败（分配、缓存、策略）均退回"现状全局阈值"语义，特性可一键开关；最坏情形仅影响"何时编译"（性能），不影响编译产物正确性。
- 并发：所有跨线程字段经 release/acquire 或 relaxed 原子访问，遵守 `CodeExtra` 既有 FT 契约（`code_extra.h:26`）。

---

# 15 上游可信源对照（实现锚点）

| 实现点 | 锚点（已核实） |
|---|---|
| gate 注入 | `cinderx/Jit/pyjit.cpp:183` `jitVectorcall`（函数起点；阈值门在 `:197`）；`:101` `countCalls` |
| `PYTHONJITAUTO` 启用 | `cinderx/Jit/pyjit.cpp:300` `jit-auto`/`PYTHONJITAUTO` 注册；`cinderx/Jit/jit_flag_processor.h:84` `addOption` 的 `void(const std::string&)` 重载（支撑 `=auto[:N]` 解析，T2.3） |
| `PYTHONJITAUTO` parser 兼容边界 | `cinderx/Jit/jit_flag_processor.cpp`：空 X-option 视为 1，空 env 不等价于 1；malformed/overflow 需保持字段原值 |
| 公有 opcode 取值 | `cinderx/Jit/bytecode.cpp:106` `BytecodeInstruction::opcode()`（已 unspecialize + 复合 `EXTENDED_OPCODE_FLAG`）；`:153` `specializedOpcode()`（特化判定）。**勿用** `private` 的 `uninstrumentedOpcode()`（`bytecode.h:57` private） |
| 指令遍历 | `cinderx/Jit/bytecode.h:102/111/163` `BytecodeInstructionBlock`/`Iterator`/`begin` |
| SP opcode 编码 | `cinderx/Interpreter/3.14/cinder_opcode_ids.h` `EXTENDED_OPCODE_FLAG=0x200`，SP opcode = `(n\|flag)` ≥512（T1.1 依据） |
| 后向边 | `cinderx/Jit/osr.cpp:327` / `osr.h:159` `collectBackedgeTargetOffsets`（仅 target、去重、上限 16）——**不提供 `{source,target}`**，故 loop_score 在本扫描内就地收集端点（T1.3） |
| 缓存载体 | `cinderx/Common/code_extra.h:12` `CodeExtra`（既有 `calls` 用 `_Py_atomic_add_uint64`/`_load_uint64_relaxed`，非 release/acquire） |
| 发布范式 | release/acquire 范式取自 `cinderx/Jit/context.cpp:523` `_Py_atomic_store_ptr_release`（`jit_compiled` 指针发布）——**`code_extra.h` 本身只描述、不实现该范式**；新 `skey_word` 沿用此 jit_compiled 范式。32 位 acquire/release helper 名称需在目标构建中核对；若 `_Py_atomic_load_uint32_acquire`/`_store_uint32_release` 不存在，使用等价 helper 或封装本特性专用 helper |
| get-or-create | `cinderx/Common/code.cpp:185` `codeExtra` + `CriticalSectionGuard`（`:200`）；NULL-on-failure |
| SP/flags / 可达性 | `cinderx/Jit/hir/preload.cpp:449` `CI_CO_STATICALLY_COMPILED`；`pyjit.cpp:96` `required_code_flags`；`pyjit.cpp:1160/1199/4025` eligibility/compile 前均拒绝缺 flags code，支撑 `InitCodeDiagnostic` 不进 v1 gate |
| import signal provider 候选挂点 | `cinderx/Interpreter/3.14/Includes/generated_cases.c.h` 与 `3.15/Includes/generated_cases.c.h` 的 `IMPORT_NAME` / `IMPORT_FROM` 调用 import C 入口；实际挂点以目标版本 import machinery 链路核对为准 |
| 特化弱语义 | `cinderx/Interpreter/3.14/Includes/ceval_macros.h` `DEOPT_IF`/`backoff_counter` |
| 上游需求/功能设计 | `docs/design/autojit-behavior-classification/【需求分析】AutoJIT 行为模式分类.md`；`docs/design/autojit-behavior-classification/【功能设计】AutoJIT 行为模式分类.md` |
| Phase 0 C++ evidence | `scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md`、`summary-clean/summary.json`、`logs/autojit-phase0-gdb-debug-container-20260602-115858.log`、`logs/autojit-phase0-gdb-after-fix-20260602-120011.log` |
