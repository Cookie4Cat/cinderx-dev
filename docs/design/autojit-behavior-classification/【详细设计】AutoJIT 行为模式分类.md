# 详细设计说明书 — 自适应 AutoJIT 行为模式分类

## 1 产品版本&密级

| 项 | 内容 |
|---|---|
| 产品 | CinderX JIT（**目标 Python 3.14**，含 Static Python；含 `Py_GIL_DISABLED` 自由线程构建） |
| 特性 | 自适应 AutoJIT 行为模式分类（Behavior Pattern Classification） |
| 版本 | v0.2（草案） |
| 密级 | 内部公开 |
| 运行环境 | CinderX 进程内 JIT；x86-64 与 ARM64（Kunpeng）；C++17 |

## 2 拟制信息

| 角色 | 信息 |
|---|---|
| 拟制 | @sisibeloved |
| 日期 | 2026-06-01 |
| 上游功能设计 | `docs/design/autojit-behavior-classification/【功能设计】AutoJIT 行为模式分类.md` |
| 上游需求 | `docs/design/autojit-behavior-classification/【需求分析】AutoJIT 行为模式分类.md` |

## 3 修订记录

| 版本 | 日期 | 修订人 | 修订说明 |
|---|---|---|---|
| v0.1 | 2026-06-01 | @sisibeloved | 首版。落地 5 个实现单元的数据结构、算法、行为/异常模型、内部接口与代码实现要点（C++）。 |
| v0.2 | 2026-06-02 | @sisibeloved | 根据 Phase 0 C++ dump 与 gdb 定位更新实现约束：分类 schema/evidence 可冻结，`startup_phase` 来源必须改为安全 import signal provider，禁止在 `jitVectorcall` 中遍历 frame/code metadata；policy/default 需 A/B release gate。 |
| v0.3 | 2026-06-02 | @sisibeloved | 与功能设计 v0.6 同步：§13 `jitVectorcall` 伪代码加入 `calls >= global` 才分类的短路（正收益前提，被扫描函数数 ~416k→~30k）；§14.3 补"被扫描函数数有界"分析并回指功能设计 §8.8.1 收益模型；§10.2.2 补 loop_score 4 档提前饱和的可选优化说明。 |
| v0.4 | 2026-06-02 | @sisibeloved | ce-doc-review 回灌：§13.2 废弃 `kNoAutoJit` 哨兵，改用 `compile_after_n_calls.has_value()` 显式分流（无阈值=即时编译，消除 interpret-forever 回归）；§13.6 更正 PYTHONJITAUTO 注册重载为 `void(int)`→`void(const std::string&)`；§13.6 给 `kDeferThresholdFactor` 补 bootstrap ×8；§10.6 曾给旧版 risk/code-size/synthetic 阈值补 bootstrap 值并纳入 T3.11 冻结契约。 |
| v0.5 | 2026-06-02 | @sisibeloved | Open Question 决策 1 回灌：实现验收拆成 provider 前 / provider 后两档；provider 前允许验证分类基建与非 startup 最小策略，provider 后才启用 startup-init 分支并声明 ImportInit 收益。 |
| v0.6 | 2026-06-02 | @sisibeloved | Open Question 决策 2 回灌：移除 v1 主线中的 `spec_band` 字段注释、`isSpecializableOpcode` 声明、`Signature` Phase-3 计数字段与完整特化观测伪代码；§11 改为 Phase-3 参考边界。 |
| v0.7 | 2026-06-02 | @sisibeloved | Open Question 决策 3 回灌：`computeThreshold` 的 startup-init 规则受限纳入 Mixed，仅 `mixed_shape` top-2 均为 Dynamic/Dispatch/Object/Control 时启用；含 Compute/Suspend 的 Mixed 不纳入。 |
| v0.8 | 2026-06-02 | @sisibeloved | Open Question 决策 4 回灌：v1 不承诺完整 ROI 预测；§14.3 补 mis-defer 守门，要求被后移 top candidate 证明省下的静态成本大于丢掉的动态收益，否则收窄或禁用对应分支。 |
| v0.9 | 2026-06-02 | @sisibeloved | Open Question 决策 5 回灌：bootstrap defaults 作为 coding/experiment defaults 进入实现和实验，`auto[:N]` 保持 opt-in；生产 policy/default 不在设计期冻结，需 A/B、相邻配置、mis-defer 与 provider 后 startup A/B。 |
| v0.10 | 2026-06-02 | @sisibeloved | ce-doc-review 决策 1 回灌：provider 前验收只要求 opt-in `auto[:N]` vs `N` 非 startup A/B；生产默认值冻结另做相邻参数比较、mis-defer 和 provider 后 startup A/B。 |
| v0.11 | 2026-06-02 | @sisibeloved | ce-doc-review 批处理回灌：签名扫描伪代码改为 `std::array + count`，补 `autojit_config_id`、`auto_classify` 状态转换、provider gate 量化线、provider-only startup 基线与 mis-defer 测量协议。 |
| v0.12 | 2026-06-02 | @sisibeloved | 与功能设计 v0.15 同步 ROI 口径：risk 统一为成本的不确定/尾部项，§14.3 验收补充静态成本、动态成本、动态收益与 `warmups=3` 测量边界。 |
| v0.13 | 2026-06-02 | @sisibeloved | 补充 import signal provider 实现边界：现有 import 执行点可作为挂点，但生产 `startup_phase/import_depth` 状态不存在，需要修改 CPython/CinderX import 路径新增轻量 provider，JIT 热路径只 O(1) 读取。 |
| v0.14 | 2026-06-02 | @sisibeloved | 与功能设计 v0.20 同步：落地 3.14 + CinderX 283/283 全量 opcode 分类表，新增 `neutral/ignored` 精确语义，固定 family/Mixed/loop/risk/synthetic 判定常量与覆盖单测契约。 |
| v0.15 | 2026-06-03 | @sisibeloved | ce-doc-review 决策回灌：v1 目标收敛为 Python 3.14-only；`StructureKey` payload 改为 valid+24bit，新增 `risk_reason` 与 `code_size_bucket`；unknown opcode fail-closed；`deriveStructureKey`/缓存返回 optional；`computeThreshold` 返回 `{limit, branch_reason}`，provider-before 只启用 low_roi/risk-defer。 |
| v0.16 | 2026-06-04 | @sisibeloved | 补充 AutoJIT 入口激活契约与回归测试缺口：所有设置 `compile_after_n_calls` 的入口都必须安装 frame evaluator；详细伪代码明确初始化顺序，验收新增 env/X-option 下新定义函数计数并触发 JIT。 |
| v0.17 | 2026-06-05 | @sisibeloved | 根据 `2to3` 穿刺更新可编码策略：payload 低 5 位改为 `active_dim_mask`；新增 `computeDominantHint()`；import/setup 分支改为高成本非数值候选；补 opt-in `lib2to3_main` setup provider。 |
| v0.18 | 2026-06-09 | @sisibeloved | 同步 import/setup split-only 实现：`GateContext` 增加 `import_phase`/`setup_phase` 诊断位，`startup_phase` 为合并位；当前 `computeThreshold` 仍只按合并位执行，分叉策略另需 A/B 证据。 |
| v0.19 | 2026-06-10 | @sisibeloved | 新增实现设计 6：负 ROI 动态反馈与退避（RoiBackoff，需求 KD9/R28–R31，功能设计 §8.8）；`CodeExtra` 增量字段、deopt 出口挂点、退避状态机、gate/OSR 集成与 P1/P2 核实清单；`BranchReason` 新增 `RoiBackoff`；原 §14/§15 顺延为 §15/§16，DFX 补 RoiBackoff 行与 V7 验收。 |

## 4 Keywords 关键词

structure_key 打包、opcode 全量表、loop_score、Phase-3 特化观测、CodeExtra 原子发布、release/acquire、jitVectorcall、computeThreshold、RoiBackoff、deopt 退避、uncompile。

## 5 Abstract 摘要

本详细设计将功能设计落到可指导编码的 C++ 实现：(1) 数据结构与编码——`StructureKey` 打包为 24-bit payload、`CodeExtra` 扩展一个 32-bit 原子发布字 `skey_word`、`opcode→OpcodeClass` 全量表；(2) 单次字节码扫描与 `structure_key` 派生算法；(3) `CodeExtra` 的 free-threaded 单字 release/acquire 发布与失败回退；(4) `jitVectorcall` 集成与**最小阈值策略** `computeThreshold`（自由函数，返回 `{limit, branch_reason}`，对明确 `raise_threshold_candidate` 抬阈值削减 compile storm，T3.1b/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11/T2.1）；(5) AutoJIT 入口激活顺序：设置 `compile_after_n_calls` 后必须安装 frame evaluator，不能只写配置。特化观测与滞回带只保留为 Phase-3 参考边界，不形成 v1 字段、接口或伪代码主线。2026-06-02 Phase 0 C++ clean summary 已冻结 gate-side 分类 schema/evidence；bootstrap cutoff/floor/δ/loop/risk 和 `kDeferThresholdFactor` 可作为 coding/experiment defaults 进入实现和实验，但未冻结生产 policy/default，`PYTHONJITAUTO=auto[:N]` 在生产默认冻结前保持 opt-in。生产推荐默认值需 `auto[:N]` vs 数值 `N` 的 A/B、相邻配置比较、mis-defer 守门和 provider A/B 后才能发布。v1 目标仅 Python 3.14；运行时遇到表外 opcode 必须 fail-closed 返回 `nullopt` 并回退全局阈值，不能当 `Neutral`。gdb 定位证明在 `jitVectorcall` 中遍历 Python frame/code metadata 计算 `import_stack` 会 SIGSEGV，故 `startup_phase` 必须来自安全 provider 或 CinderX-only wrapper provider 的 O(1) depth/bool，热路径只消费 provider 输出。`2to3` 穿刺证明 import depth 只能覆盖 import 阶段，`lib2to3.main.main()` 的 refactor/setup 窗口需要单独 provider；当前实现新增 opt-in `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main` 验证该窗口。v1 不承诺完整 ROI 预测精度，只削减明确低收益/高成本形态；被后移的 top call-count / top time candidate 必须通过 mis-defer 守门。import/setup 分支的默认条件是 `startup_phase && !is_static && !computeDominantHint() && (risk_reason != 0 || code_size_bucket > 0)`；compute-dominant 只认 `NumericLoop` 或 `Mixed` top-2 含 Compute，incidental `Compute` 不保护对象/控制/分发主族。`high_risk` 由 `risk_reason != 0` 派生；`risk_reason` 记录 suspend/dynamic/exception/huge-code 四类来源，`code_size_bucket` 记录 `<50`、`50-199`、`200-499`、`>=500` 四档，`active_dim_mask` 记录非零工作维度，支撑 risk-defer/import-setup 失败后的精确收窄。synthetic 低 ROI 默认只覆盖无 loop、非 static、ReflectionMeta/Trivial；`Mixed` 通过 `mixed_shape` 保留 top-2 维度组合；剩余并列选族采用固定排序键 `bucket desc -> dim_count desc -> benefit-first`；分类配置进程内冻结且缓存无运行期失效；字符串仅用于诊断解码。设计以单字原子发布消除"值/标志"排序风险，并对每个 v1 单元给出正常/异常行为模型、数据流转、内部接口定义与 DFX。2026-06-10 新增实现设计 6（v1.5 RoiBackoff，§14）：在既有 deopt 出口按 `DeoptReason` mask 做 O(1) 计数，deopt 风暴函数经 `jit::uncompile` 收回、重编译下限指数加价、超轮次后置 `DECIDED_COLD` 冷位冻结；状态存 `CodeExtra` 增量字段、不进 `structure_key`，默认关闭、独立于 `auto_classify`；P1（机器码生命周期）/P2（共享 code 全量入口解除）核实与 gdb smoke 通过前不得默认开启。

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

本文档面向实现者，给出 CinderX 行为分类器的内部接口与代码实现参考，具体到 C++17 与 CinderX 运行环境。新增代码集中于 `cinderx/Jit/behavior_classifier.{h,cpp}`，并对 `cinderx/Common/code_extra.h`、`cinderx/Jit/pyjit.cpp` 做受控改动；v1.5 RoiBackoff（§14）另对 deopt 出口（`cinderx/Jit/codegen/gen_asm.cpp` `prepareForDeopt`）与 OSR 准入（`cinderx/Jit/osr.cpp` `osrCompileBudgetCheck`）做受控改动。所有跨线程字段均按 CinderX 既有原子范式实现（参照 `context.cpp` 的 `jit_compiled` release/acquire 发布）。本文沿用功能设计的 v1 术语：`structure_key`（确定性聚合身份）与 `gate_context`（当次 gate 上下文，不聚合）。`specialization_band` 仅作为 §11 的 Phase-3 参考术语，不属于 v1 接口。

---

# 8 上游文档引用

| 上游 | 关键约束（本详细设计须满足） |
|---|---|
| 需求 R18 | `structure_key` 是唯一聚合键；`gate_context` 不进键、不聚合；Phase-3 的 `specialization_band` 也不得进键 |
| 需求 R20 | `structure_key` 确定；band 带滞回 |
| 需求 R21/R26、KD8 | 单次 O(n) 扫描、缓存、FT release/acquire 发布、失败回退默认阈值 |
| 需求 R22/R25 | 归一遍历，每 opcode 唯一归属；band 旁路采集互不污染 |
| 需求 KD9、R28–R31、L6 | RoiBackoff 状态存 `CodeExtra` 不进键；deopt 出口 O(1) 观测；退避动作复用既有机制；开关关闭 bit-for-bit 等价 |
| 功能设计 8.4–8.8 | 5 功能项的逻辑接口与调用路径（8.8 为 v1.5 RoiBackoff） |

---

# 9 实现设计 1：数据结构与编码

## 9.1 实现概述

定义三组数据：`StructureKey` 的位打包、`CodeExtra` 的扩展字段（单字原子发布）、`opcode→OpcodeClass` 全量表。这是其余单元的公共底座。

## 9.2 关键算法与流程

**术语对应（审校 T4.2/T3.10）：** 上游需求/功能设计中的 `structure_key` 指**逻辑聚合身份**（解码后的 `StructureKey` 值）；本详细设计的 `skey_word` 指其**物理容器**（含 valid 位 + payload 的 32 bit 字）。凡缓存、统计、聚合契约中说"structure_key"，一律指解码值，绝不指原始字。字符串只在 Phase 0 dump、日志和诊断中由 payload 解码生成，不进入热路径、缓存或聚合主表示。`autojit_config_id` 是 dump/log/report 层的配置 hash，不写入 `skey_word`，用于阻止不同 schema/policy 配置的实验结果被误合并。数据流：`StructureKey`(值) → `pack()` → payload → `skey_word`(release 写) → acquire 读 → `unpack()` → `StructureKey`(值)。

`StructureKey` 打包为一个 ≤24 bit 的 payload，再与一个 `valid` 位组成 **单个 32 bit 发布字** `skey_word`。关键点：**用单字单次 release/acquire 发布**，从根本上消除"先写值后写标志"的排序风险（审查 Finding 3 的最稳妥实现）。

```
skey_word (uint32_t) 位布局：
  bit 31        valid           # 1=已分类（PyMem_Calloc 零初始化 => 0=未分类）
  bits [20..23] mixed_shape     # 0=none；1..15=Mixed canonical top-2 工作维度组合（T3.7）
  bits [16..19] family          # Family 枚举，8 个值（T2.4 去 ScalarCompute；T3.4 去 ImportInit），占 4 bit
  bits [14..15] loop_score      # 0..3
  bit  13       is_suspendable
  bit  12       is_static
  bit  11       is_synthetic
  bits [7..10]  risk_reason     # bit0=suspend, bit1=dynamic, bit2=exception, bit3=huge_code
  bits [5..6]   code_size_bucket# 0:<50, 1:50-199, 2:200-499, 3:>=500
  bits [0..4]   active_dim_mask # bit0=Compute, bit1=Control, bit2=Object, bit3=Dispatch, bit4=Dynamic
PAYLOAD_MASK = 0x00FF_FFFF ; VALID_BIT = 0x8000_0000
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
  // 审校 T2.4：去 ScalarCompute——first.dim == Compute 一律 NumericLoop，由 loop_score 区分有无循环
  NumericLoop = 0, BranchFSM, ObjectManipulator,
  CallDispatcher, AsyncStateMachine, ReflectionMeta,
  Trivial, Mixed,
  kCount
};
// kCount==8，须与需求 R15 的 family 枚举逐一对应（新增 family 须同步改 R15 与位宽）
static_assert(static_cast<int>(Family::kCount) == 8, "family count must match requirements R15");
static_assert(static_cast<int>(Family::kCount) <= 16, "family must fit in 4 bits");

enum class WorkDim : uint8_t {
  Compute = 0, Control, Object, Dispatch, Suspend, Dynamic, kCount
};

enum class OpcodeClass : uint8_t {
  Compute = 0, Control, Object, Dispatch, Suspend, Dynamic,
  Neutral,  // 计入 effective_instruction_count，不递增维度计数
  Ignored,  // 不计入 effective_instruction_count，也不递增维度计数
  Invalid   // 表外 opcode；运行时 fail-closed，回退全局阈值
};

constexpr bool isWorkDim(OpcodeClass c) {
  return static_cast<uint8_t>(c) < static_cast<uint8_t>(OpcodeClass::Neutral);
}
WorkDim toWorkDim(OpcodeClass c);  // 仅当 isWorkDim(c) 为 true 时调用

// 0=非 Mixed；1..15 为 6 个 WorkDim 的 canonical unordered pair 编码（T3.7）
using MixedShape = uint8_t;
constexpr MixedShape kMixedShapeNone = 0;

enum RiskReason : uint8_t {
  kRiskNone      = 0,
  kRiskSuspend   = 1u << 0,
  kRiskDynamic   = 1u << 1,
  kRiskException = 1u << 2,
  kRiskHugeCode  = 1u << 3,
};

// 解析后的结构身份（值类型，可平凡拷贝）
struct StructureKey {
  Family   family{Family::Trivial};
  MixedShape mixed_shape{kMixedShapeNone};
  uint8_t  loop_score{0};        // 0..3
  bool     is_suspendable{false};
  bool     is_static{false};
  bool     is_synthetic{false};
  uint8_t  risk_reason{kRiskNone};   // RiskReason bitset；high_risk = risk_reason != 0
  uint8_t  code_size_bucket{0};      // 0:<50, 1:50-199, 2:200-499, 3:>=500
  uint8_t  active_dim_mask{0};       // 非零 bucket 的工作维度；不含 Suspend

  bool highRisk() const { return risk_reason != kRiskNone; }
  bool hasActiveDim(WorkDim dim) const;
  bool computeHint() const;          // active_dim_mask 中是否出现 Compute
  bool computeDominantHint() const;  // NumericLoop 或 Mixed top-2 含 Compute
  uint32_t pack() const;                       // -> 24-bit payload (无 valid 位)
  static StructureKey unpack(uint32_t payload);
};

constexpr uint32_t kSkeyValidBit = 0x80000000u;
constexpr uint32_t kSkeyPayloadMask = 0x00FFFFFFu;
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
  uint32_t skey_word;   /* bit31=valid; 低24位=StructureKey payload；零初始化=未分类 */
} CodeExtra;
```

### 9.4.2 数据流转

`scanCode`(单元 10) → `StructureKey`(值) → `pack()` → `payload | VALID_BIT` → 单字 release 发布进 `skey_word`(单元 12) → gate 读路径 acquire 加载 → `unpack()` → 供 `computeThreshold`(单元 13)。v1 `CodeExtra` 不包含特化观测字段。

## 9.5 接口设计

### 9.5.1 内部接口设计

公共底座接口：`StructureKey::pack/unpack`、全量表查询 `opcodeClassOf(opcode)`。均为无副作用纯函数，供其它单元调用。

### 9.5.2 内部接口定义

```cpp
uint32_t StructureKey::pack() const;             // 24-bit payload；不得设置 valid 位
StructureKey StructureKey::unpack(uint32_t payload);
MixedShape encodeMixedShape(WorkDim a, WorkDim b);  // canonical unordered pair；非 Mixed 使用 kMixedShapeNone
OpcodeClass opcodeClassOf(int canonical_opcode);     // 全量表查询；未知返回 Invalid，调用方 fail-closed
bool isExceptionControlOpcode(int canonical_opcode);
uint8_t activeDimMaskFor(WorkDim dim);
```

## 9.6 代码实现要点

全量表按 opcode name 冻结，实现按 opcode id 生成 `OpcodeClass` 查表。输入全集固定为目标 3.14 运行时的 opcode 名称集合：CPython 3.14 `opcode.opmap` 154 条、`opcode._specialized_opmap` 84 条、CinderX 3.14 `cinder_opcode_ids.h` 扩展 43 条，加上 `cinder_opcode.h` 固定的 `EAGER_IMPORT_NAME` / `EXTENDED_OPCODE` 2 条；合计 **283 条唯一 opcode 名称**。`opcode_stubs.h` 中为跨版本兼容定义的 out-of-range stub 不属于 3.14 可执行 opcode 输入集，不能混入此表。

| 查表结果 | 数量 | opcode 全量清单 |
|---|---:|---|
| `compute` | 31 | `BINARY_OP`, `BINARY_OP_ADD_FLOAT`, `BINARY_OP_ADD_INT`, `BINARY_OP_ADD_UNICODE`, `BINARY_OP_EXTEND`<br/>`BINARY_OP_INPLACE_ADD_UNICODE`, `BINARY_OP_MULTIPLY_FLOAT`, `BINARY_OP_MULTIPLY_INT`, `BINARY_OP_SUBTRACT_FLOAT`<br/>`BINARY_OP_SUBTRACT_INT`, `CAST`, `CAST_CACHED`, `COMPARE_OP`, `COMPARE_OP_FLOAT`, `COMPARE_OP_INT`, `COMPARE_OP_STR`<br/>`CONTAINS_OP`, `CONTAINS_OP_DICT`, `CONTAINS_OP_SET`, `CONVERT_PRIMITIVE`, `IS_OP`, `LOAD_TYPE`, `PRIMITIVE_BINARY_OP`<br/>`PRIMITIVE_BOX`, `PRIMITIVE_COMPARE_OP`, `PRIMITIVE_UNARY_OP`, `PRIMITIVE_UNBOX`, `REFINE_TYPE`, `UNARY_INVERT`<br/>`UNARY_NEGATIVE`, `UNARY_NOT` |
| `control` | 54 | `CHECK_EG_MATCH`, `CHECK_EXC_MATCH`, `CLEANUP_THROW`, `END_FOR`, `EXIT_INIT_CHECK`, `FOR_ITER`, `FOR_ITER_GEN`<br/>`FOR_ITER_LIST`, `FOR_ITER_RANGE`, `FOR_ITER_TUPLE`, `INSTRUMENTED_END_FOR`, `INSTRUMENTED_FOR_ITER`<br/>`INSTRUMENTED_JUMP_BACKWARD`, `INSTRUMENTED_JUMP_FORWARD`, `INSTRUMENTED_NOT_TAKEN`, `INSTRUMENTED_POP_JUMP_IF_FALSE`<br/>`INSTRUMENTED_POP_JUMP_IF_NONE`, `INSTRUMENTED_POP_JUMP_IF_NOT_NONE`, `INSTRUMENTED_POP_JUMP_IF_TRUE`<br/>`INSTRUMENTED_RETURN_VALUE`, `JUMP`, `JUMP_BACKWARD`, `JUMP_BACKWARD_JIT`, `JUMP_BACKWARD_NO_INTERRUPT`<br/>`JUMP_BACKWARD_NO_JIT`, `JUMP_FORWARD`, `JUMP_IF_FALSE`, `JUMP_IF_TRUE`, `JUMP_NO_INTERRUPT`, `NOT_TAKEN`, `POP_BLOCK`<br/>`POP_EXCEPT`, `POP_JUMP_IF_FALSE`, `POP_JUMP_IF_NONE`, `POP_JUMP_IF_NONZERO`, `POP_JUMP_IF_NOT_NONE`<br/>`POP_JUMP_IF_TRUE`, `POP_JUMP_IF_ZERO`, `PUSH_EXC_INFO`, `RAISE_VARARGS`, `RERAISE`, `RETURN_PRIMITIVE`<br/>`RETURN_VALUE`, `SETUP_CLEANUP`, `SETUP_FINALLY`, `SETUP_WITH`, `TO_BOOL`, `TO_BOOL_ALWAYS_TRUE`, `TO_BOOL_BOOL`<br/>`TO_BOOL_INT`, `TO_BOOL_LIST`, `TO_BOOL_NONE`, `TO_BOOL_STR`, `WITH_EXCEPT_START` |
| `object` | 72 | `BINARY_OP_SUBSCR_DICT`, `BINARY_OP_SUBSCR_GETITEM`, `BINARY_OP_SUBSCR_LIST_INT`, `BINARY_OP_SUBSCR_LIST_SLICE`<br/>`BINARY_OP_SUBSCR_STR_INT`, `BINARY_OP_SUBSCR_TUPLE_INT`, `BINARY_SLICE`, `BUILD_CHECKED_LIST`<br/>`BUILD_CHECKED_LIST_CACHED`, `BUILD_CHECKED_MAP`, `BUILD_CHECKED_MAP_CACHED`, `BUILD_LIST`, `BUILD_MAP`, `BUILD_SET`<br/>`BUILD_SLICE`, `BUILD_TUPLE`, `DELETE_ATTR`, `DELETE_SUBSCR`, `DICT_MERGE`, `DICT_UPDATE`, `FAST_LEN`, `GET_ITER`<br/>`GET_LEN`, `LIST_APPEND`, `LIST_DEL`, `LIST_EXTEND`, `LOAD_ATTR`, `LOAD_ATTR_CLASS`<br/>`LOAD_ATTR_CLASS_WITH_METACLASS_CHECK`, `LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN`, `LOAD_ATTR_INSTANCE_VALUE`<br/>`LOAD_ATTR_METHOD_LAZY_DICT`, `LOAD_ATTR_METHOD_NO_DICT`, `LOAD_ATTR_METHOD_WITH_VALUES`, `LOAD_ATTR_MODULE`<br/>`LOAD_ATTR_NONDESCRIPTOR_NO_DICT`, `LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES`, `LOAD_ATTR_PROPERTY`, `LOAD_ATTR_SLOT`<br/>`LOAD_ATTR_WITH_HINT`, `LOAD_FIELD`, `LOAD_ITERABLE_ARG`, `LOAD_MAPPING_ARG`, `LOAD_OBJ_FIELD`, `LOAD_PRIMITIVE_FIELD`<br/>`MAP_ADD`, `MATCH_CLASS`, `MATCH_KEYS`, `MATCH_MAPPING`, `MATCH_SEQUENCE`, `SEQUENCE_GET`, `SEQUENCE_SET`, `SET_ADD`<br/>`SET_UPDATE`, `STORE_ATTR`, `STORE_ATTR_INSTANCE_VALUE`, `STORE_ATTR_SLOT`, `STORE_ATTR_WITH_HINT`, `STORE_FIELD`<br/>`STORE_OBJ_FIELD`, `STORE_PRIMITIVE_FIELD`, `STORE_SLICE`, `STORE_SUBSCR`, `STORE_SUBSCR_DICT`<br/>`STORE_SUBSCR_LIST_INT`, `TP_ALLOC`, `TP_ALLOC_CACHED`, `UNPACK_EX`, `UNPACK_SEQUENCE`, `UNPACK_SEQUENCE_LIST`<br/>`UNPACK_SEQUENCE_TUPLE`, `UNPACK_SEQUENCE_TWO_TUPLE` |
| `dispatch` | 44 | `CALL`, `CALL_ALLOC_AND_ENTER_INIT`, `CALL_BOUND_METHOD_EXACT_ARGS`, `CALL_BOUND_METHOD_GENERAL`, `CALL_BUILTIN_CLASS`<br/>`CALL_BUILTIN_FAST`, `CALL_BUILTIN_FAST_WITH_KEYWORDS`, `CALL_BUILTIN_O`, `CALL_FUNCTION_EX`, `CALL_INTRINSIC_1`<br/>`CALL_INTRINSIC_2`, `CALL_ISINSTANCE`, `CALL_KW`, `CALL_KW_BOUND_METHOD`, `CALL_KW_NON_PY`, `CALL_KW_PY`, `CALL_LEN`<br/>`CALL_LIST_APPEND`, `CALL_METHOD_DESCRIPTOR_FAST`, `CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS`<br/>`CALL_METHOD_DESCRIPTOR_NOARGS`, `CALL_METHOD_DESCRIPTOR_O`, `CALL_NON_PY_GENERAL`, `CALL_PY_EXACT_ARGS`<br/>`CALL_PY_GENERAL`, `CALL_STR_1`, `CALL_TUPLE_1`, `CALL_TYPE_1`, `INSTRUMENTED_CALL`, `INSTRUMENTED_CALL_FUNCTION_EX`<br/>`INSTRUMENTED_CALL_KW`, `INSTRUMENTED_LOAD_SUPER_ATTR`, `INVOKE_FUNCTION`, `INVOKE_FUNCTION_CACHED`<br/>`INVOKE_INDIRECT_CACHED`, `INVOKE_METHOD`, `INVOKE_NATIVE`, `LOAD_METHOD_STATIC`, `LOAD_METHOD_STATIC_CACHED`<br/>`LOAD_SPECIAL`, `LOAD_SUPER_ATTR`, `LOAD_SUPER_ATTR_ATTR`, `LOAD_SUPER_ATTR_METHOD`, `PUSH_NULL` |
| `suspend` | 13 | `END_ASYNC_FOR`, `END_SEND`, `GET_AITER`, `GET_ANEXT`, `GET_AWAITABLE`, `GET_YIELD_FROM_ITER`<br/>`INSTRUMENTED_END_ASYNC_FOR`, `INSTRUMENTED_END_SEND`, `INSTRUMENTED_YIELD_VALUE`, `RETURN_GENERATOR`, `SEND`<br/>`SEND_GEN`, `YIELD_VALUE` |
| `dynamic` | 32 | `ANNOTATIONS_PLACEHOLDER`, `BUILD_INTERPOLATION`, `BUILD_STRING`, `BUILD_TEMPLATE`, `CONVERT_VALUE`, `COPY_FREE_VARS`<br/>`DELETE_DEREF`, `DELETE_GLOBAL`, `DELETE_NAME`, `EAGER_IMPORT_NAME`, `FORMAT_SIMPLE`, `FORMAT_WITH_SPEC`<br/>`IMPORT_FROM`, `IMPORT_NAME`, `LOAD_BUILD_CLASS`, `LOAD_CLASS`, `LOAD_CLOSURE`, `LOAD_DEREF`<br/>`LOAD_FROM_DICT_OR_DEREF`, `LOAD_FROM_DICT_OR_GLOBALS`, `LOAD_GLOBAL`, `LOAD_GLOBAL_BUILTIN`, `LOAD_GLOBAL_MODULE`<br/>`LOAD_LOCALS`, `LOAD_NAME`, `MAKE_CELL`, `MAKE_FUNCTION`, `SETUP_ANNOTATIONS`, `SET_FUNCTION_ATTRIBUTE`, `STORE_DEREF`<br/>`STORE_GLOBAL`, `STORE_NAME` |
| `neutral` | 26 | `COPY`, `DELETE_FAST`, `INSTRUMENTED_POP_ITER`, `INTERPRETER_EXIT`, `LOAD_COMMON_CONSTANT`, `LOAD_CONST`<br/>`LOAD_CONST_IMMORTAL`, `LOAD_CONST_MORTAL`, `LOAD_FAST`, `LOAD_FAST_AND_CLEAR`, `LOAD_FAST_BORROW`<br/>`LOAD_FAST_BORROW_LOAD_FAST_BORROW`, `LOAD_FAST_CHECK`, `LOAD_FAST_LOAD_FAST`, `LOAD_LOCAL`, `LOAD_SMALL_INT`<br/>`POP_ITER`, `POP_TOP`, `PRIMITIVE_LOAD_CONST`, `STORE_FAST`, `STORE_FAST_LOAD_FAST`, `STORE_FAST_MAYBE_NULL`<br/>`STORE_FAST_STORE_FAST`, `STORE_LOCAL`, `STORE_LOCAL_CACHED`, `SWAP` |
| `ignored` | 11 | `CACHE`, `ENTER_EXECUTOR`, `EXTENDED_ARG`, `EXTENDED_OPCODE`, `INSTRUMENTED_INSTRUCTION`, `INSTRUMENTED_LINE`<br/>`INSTRUMENTED_RESUME`, `NOP`, `RESERVED`, `RESUME`, `RESUME_CHECK` |

实现要点：

- **入参必须是 `BytecodeInstruction::opcode()` 的返回值**（它已 unspecialize 且对 SP 复合了 `EXTENDED_OPCODE_FLAG`），不能用 `uninstrumentedOpcode()` 的原始字节（见单元 10.2.1 修正）。
- 运行时可用两层 constexpr 表（基础 opcode 0..255 + SP opcode `op & 0xFF`）或生成的 flat table；无论使用哪种结构，都必须由上表生成/校验，不能依赖前缀启发式。表外 opcode 返回 `OpcodeClass::Invalid`；扫描立即返回 `nullopt`，gate 回退全局阈值，不能把未知 opcode 当 `Neutral` 静默低估。
- `OpcodeClass::Ignored` 不计入 `effective_instruction_count`；`OpcodeClass::Neutral` 计入 `effective_instruction_count`，但不递增任何维度。
- 全量覆盖单测必须从 CPython 3.14 `opcode.opmap`、`opcode._specialized_opmap`、`cinder_opcode_ids.h` 和 `EAGER_IMPORT_NAME/EXTENDED_OPCODE` 反向生成期望全集，断言文档/实现表 **seen=283、unique=283、missing=0、extra=0、duplicate=0**。
- 语义 golden 测试固定至少覆盖：`LOAD_GLOBAL→Dynamic`、`CALL→Dispatch`、`BUILD_STRING→Dynamic`、`TO_BOOL→Control`、`SEND→Suspend`、`LOAD_ATTR→Object`、`PRIMITIVE_BINARY_OP→Compute`、`CACHE→Ignored`、`LOAD_CONST→Neutral`、异常 opcode→Control。

---

# 10 实现设计 2：签名扫描与 structure_key 派生

## 10.1 实现概述

对 code object 单次遍历 `BytecodeInstructionBlock`，累积 6 工作维度计数、异常子计数，并在同一遍历中就地收集后向边端点求 `loop_score`，最终派生 `StructureKey`。对应功能项 1（R1–R10、R12–R20、R22–R25）。v1 扫描不统计可特化/已特化计数。

## 10.2 关键算法与流程

### 10.2.1 单次扫描

**单次遍历同时完成两件事**：工作维度计数、**后向边端点收集**（审校修正 T1.3：既有 OSR 只暴露 `collectBackedgeTargetOffsets`（仅 target、去重、上限 16），不提供 `{source,target}`，故循环嵌套必须在本扫描内就地记录 `source`+`getJumpTarget()`，既消除不存在的依赖，又让"单次 O(n) 扫描"名副其实）。v1 不做特化旁路采集（审校 T2.2，特化观测 defer 到 Phase-3 参考边界）。

```cpp
struct Signature {
  std::array<uint32_t, (size_t)WorkDim::kCount> counts{};
  uint32_t exception_control_count{0};  // 异常子计数（供 risk_reason 派生）
  uint32_t n_eff{0};                    // 有效指令（分母）
  // 后向边端点 {source_idx, target_idx}，就地收集，固定上限避免堆分配
  std::array<std::pair<int,int>, CI_OSR_MAX_BACKEDGES> backedges{};
  size_t stored_backedge_count{0};
  uint32_t seen_backedge_count{0};      // 包含超过存储上限的 backedge，用于 >=16 直接饱和
  uint8_t  loop_score{0};
};

std::optional<Signature> scanCode(BorrowedRef<PyCodeObject> code) {
  Signature s;
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    int op = instr.opcode();          // 公有：已 unspecialize 且对 SP 复合 EXTENDED_OPCODE_FLAG（R22）
    OpcodeClass cls = opcodeClassOf(op);
    if (cls == OpcodeClass::Invalid) {
      return std::nullopt;             // Python minor/opcode 表不匹配；fail-closed
    }
    if (cls == OpcodeClass::Ignored) {
      continue;                       // 不计入分母
    }

    s.n_eff++;
    if (isWorkDim(cls)) {
      s.counts[(size_t)toWorkDim(cls)]++;
    }
    if (isExceptionControlOpcode(op)) {
      s.exception_control_count++;
    }

    // —— 后向边就地收集（T1.3）——
    if (instr.isBranch()) {
      BCIndex src = instr.opcodeIndex();
      BCIndex tgt = instr.getJumpTarget().asIndex(); // 与 src 同为 instruction index 坐标
      if (tgt < src) {
        if (s.stored_backedge_count < CI_OSR_MAX_BACKEDGES) {
          s.backedges[s.stored_backedge_count++] = {src.value(), tgt.value()};
        }
        s.seen_backedge_count++;
      }
    }
  }
  s.loop_score = loopScore(
      s.backedges, s.stored_backedge_count, s.seen_backedge_count); // 见 10.2.2
  return s;
}
```

### 10.2.2 loop_score（嵌套深度，就地端点）

用扫描中就地收集的后向边端点（10.2.1），将每条边视为区间 `[target_idx, source_idx]`。bootstrap `loop_score`（T3.9）取嵌套深度和多 backedge 数两路的最大值：`nesting_score = min(max_static_nesting_depth, 3)`；`count_score = 0/1/2/3` 对应 backedge 数 `0 / 1 / 2–3 / >=4`。**无第二次扫描、无堆分配**（边集为栈上 `std::array`，上限 `CI_OSR_MAX_BACKEDGES=16`）：

```cpp
uint8_t loopScore(
    const std::array<std::pair<int,int>, CI_OSR_MAX_BACKEDGES>& edges,
    size_t stored_edge_count,
    uint32_t seen_edge_count) {
  if (seen_edge_count == 0) return 0;
  if (seen_edge_count >= 16) return 3;

  // 事件法：+1 在 target，-1 在 source 之后；求最大同时覆盖数
  std::array<std::pair<int,int>, CI_OSR_MAX_BACKEDGES * 2> ev{}; // (idx, delta)
  size_t ev_count = 0;
  for (size_t i = 0; i < stored_edge_count; ++i) {
    auto [src, tgt] = edges[i];
    ev[ev_count++] = {tgt, +1};
    ev[ev_count++] = {src + 1, -1};
  }
  std::sort(ev.begin(), ev.begin() + ev_count);
  int cur = 0, depth = 0;
  for (size_t i = 0; i < ev_count; ++i) {
    auto [idx, d] = ev[i];
    cur += d;
    depth = std::max(depth, cur);
  }
  uint8_t nesting_score = (uint8_t)std::min(depth, 3);
  uint8_t count_score =
      seen_edge_count >= 4 ? 3 : (seen_edge_count >= 2 ? 2 : 1);
  return std::min<uint8_t>(3, std::max(nesting_score, count_score));
}
```

> 注：既有 `collectBackedgeTargetOffsets`（`cinderx/Jit/osr.cpp:327`）的 16 条上限语义在此保留（`stored_backedge_count < CI_OSR_MAX_BACKEDGES`），`seen_backedge_count >= 16` 直接截顶到 `loop_score=3`，对 0–3 分级无影响。

> 提前饱和（可选优化）：`loop_score ∈ {0,1,2,3}` 仅 2 bit，`count_score` 累到 `>=4`、`nesting` 达到 3 即已顶最高桶，故扫描中边数达 4 后可不再 `push_back`、`loopScore` 的扫线达深度 3 即可短路返回。该优化只省常数、不改语义，实现可选。

### 10.2.3 派生（预过滤 → 分桶 → 选族）

```cpp
bool isAutoJitClassifiable(BorrowedRef<PyCodeObject> code) {
  return existingAutoJitOrJitListEligibility(code) &&
      hasRequiredFlags(code) &&
      !isModuleName(code) &&
      (code->co_flags & CO_ASYNC_GENERATOR) == 0 &&
      (code->co_flags & CI_CO_SUPPRESS_JIT) == 0 &&
      !isCinderXInternalModule(code);
}

std::optional<StructureKey> deriveStructureKey(BorrowedRef<PyCodeObject> code) {
  StructureKey k;

  // R12/T3.4：不可分类代码只进 Phase 0 InitCodeDiagnostic 或直接回退；
  // v1 gate 热路径不应把它编码进 StructureKey。
  if (!isAutoJitClassifiable(code)) {
    return std::nullopt;
  }
  auto sig = scanCode(code);
  if (!sig.has_value()) {
    return std::nullopt;              // unknown opcode fail-closed
  }
  const Signature& s = *sig;
  auto buckets = bucketize(s);                                        // R14：COUNT_FLOOR=2, cutoff=0.10/0.25/0.50

  // 结构修饰位（与族正交）
  k.loop_score = s.loop_score;                                        // R7
  k.is_static = (code->co_flags & CI_CO_STATICALLY_COMPILED) != 0;     // R9
  k.is_suspendable =
      (code->co_flags & (CO_GENERATOR|CO_COROUTINE|CO_ASYNC_GENERATOR)) != 0 ||
      s.counts[(size_t)WorkDim::Suspend] > 0;                         // R10
  k.is_synthetic = isSyntheticFilename(code);                         // T3.6：低 ROI 与风险分离
  RiskDetail risk = deriveRisk(s, buckets);                            // R8（见 10.2.4）
  k.risk_reason = risk.reason;
  k.code_size_bucket = risk.code_size_bucket;
  k.active_dim_mask = activeDimMask(buckets);                          // 非零 bucket 维度；解释 incidental Compute

  if (allBucketsZero(buckets)) {                                      // R13
    k.family = Family::Trivial;
    k.mixed_shape = kMixedShapeNone;
    return k;
  }

  auto [d1, d2] = topTwo(
      buckets,
      s.counts,
      kBenefitFirstTieBreakOrder);                                    // R15：bucket desc -> count desc -> benefit-first
  if (bucket(buckets, d1) >= kMixedMinBucket &&
      bucket(buckets, d2) >= kMixedMinBucket &&
      bucket(buckets, d1) - bucket(buckets, d2) <= kMixedBucketDelta) { // R16
    k.family = Family::Mixed;
    k.mixed_shape = encodeMixedShape(d1, d2);                          // T3.7：保留 Mixed top-2 组合
    return k;
  }
  k.family = familyForFirstDim(d1);                                   // R15 映射表；风险由 modifier/policy 处理
  k.mixed_shape = kMixedShapeNone;
  return k;
}
```

### 10.2.4 risk 派生（不重复计 opcode，R8/R25）

```cpp
struct RiskDetail {
  uint8_t reason{kRiskNone};
  uint8_t code_size_bucket{0};
};

uint8_t codeSizeBucket(uint32_t n_eff) {
  if (n_eff >= 500) return 3;
  if (n_eff >= 200) return 2;
  if (n_eff >= 50) return 1;
  return 0;
}

RiskDetail deriveRisk(
    const Signature& s,
    const std::array<uint8_t, (size_t)WorkDim::kCount>& buckets) {
  RiskDetail r;
  if (buckets[(size_t)WorkDim::Suspend] >= kRiskSuspendBucket) {
    r.reason |= kRiskSuspend;
  }
  if (buckets[(size_t)WorkDim::Dynamic] >= kRiskDynamicBucket) {
    r.reason |= kRiskDynamic;
  }
  if (s.exception_control_count >= kRiskExceptionFloor) {
    r.reason |= kRiskException;
  }
  if (s.n_eff >= kRiskEffectiveInstructionFloor) {
    r.reason |= kRiskHugeCode;
  }
  r.code_size_bucket = codeSizeBucket(s.n_eff);
  return r;                                                                       // synthetic 独立进 is_synthetic(T3.6)
}
```

## 10.3 行为模型

### 10.3.1 正常流程

输入合法且 `isAutoJitClassifiable(code)==true` 的 code object → 单次遍历 → 返回闭集中的唯一 `Family` + 修饰位 + `risk_reason/code_size_bucket/active_dim_mask`。

### 10.3.2 异常流程

- code 无指令（空 block）：`n_eff=0` → `allBelowFloor` 真 → `Trivial`（不除零，分桶对 `n_eff==0` 短路）。
- backedge 收集返回空：`loop_score=0`，正常。
- 六个维度 bucket 全为 0（只有 neutral/ignored 或所有维度低于 floor）：`Trivial`。
- `isAutoJitClassifiable(code)==false`：返回 `nullopt`，runtime gate 回退全局阈值；Phase 0 dump 可单独输出 `InitCodeDiagnostic`。
- 运行时遇到表外 opcode：返回 `nullopt`，runtime gate 回退全局阈值；覆盖测试必须阻止 3.14 表漂移发布。
- 不抛 C++ 异常：纯计算路径，便于在 gate 上无 try/catch 调用。

## 10.4 数据模型

### 10.4.1 数据结构定义

见单元 9（`StructureKey`、`Signature`）。`bucketize` 产出固定长度 `std::array<uint8_t,6>`（每维 0..3）。

### 10.4.2 数据流转

`PyCodeObject*`(只读) → `Signature`(栈上) → `std::optional<StructureKey>`(值返回)。`Signature` 不逃逸、不堆分配（计数器为栈数组；`loopScore` 内部使用定长 `std::array` 事件缓冲，容量 `2 * CI_OSR_MAX_BACKEDGES`）。

## 10.5 接口设计

### 10.5.1 内部接口设计

`deriveStructureKey` 为单元 12（缓存）唯一调用；其余为文件内静态辅助。无副作用（仅读取 code object 字节码/flags/稳定元数据，不改 code；v1 不读取特化态）。

### 10.5.2 内部接口定义

```cpp
std::optional<StructureKey> deriveStructureKey(BorrowedRef<PyCodeObject> code);   // 纯函数；不可分类/unknown opcode 返回 nullopt
bool isAutoJitClassifiable(BorrowedRef<PyCodeObject> code);
// 文件内静态：scanCode / loopScore / bucketize / topTwo / familyForFirstDim / deriveRisk / isSyntheticFilename
```

## 10.6 代码实现要点

- 遍历用既有 `BytecodeInstructionBlock` 迭代器（`bytecode.h:111/163`），不手算偏移。
- **opcode 取值必须用公有 `BytecodeInstruction::opcode()`**（`bytecode.cpp:106`，已 unspecialize 且对 SP 复合 `EXTENDED_OPCODE_FLAG`）——**不要**用 `private` 的 `uninstrumentedOpcode()`（编译不可见，且返回未复合 flag 的 ≤255 原始字节，会漏掉全部 SP opcode，审校 T1.2）。（Phase-3 才加特化判定 `specializedOpcode() != opcode()`，与工作维度同遍历、互不污染，R22；v1 不做，T2.2。）
- 后向边在同一遍历内就地收集（10.2.1），不调用第二遍 `collectBackedgeTargetOffsets`，使"单次 O(n) 扫描"成立（审校 T1.3）。
- `bucketize` 必须对 `n_eff==0` 与低于 floor 短路，杜绝除零与微函数假信号（R14）。
- coding/experiment defaults 集中为冻结常量：`kPythonMinor=3.14`、`kOpcodeTableCoverage=283`、`kDimCountFloor=2`、density cutoff `0.10/0.25/0.50`、`kMixedBucketDelta=1`、`kMixedMinBucket=2`、tie-break order `Compute, Dispatch, Object, Control, Dynamic, Suspend`、loop count score 阈值 `0/1/2-3/>=4`、`kRiskSuspendBucket=2`、`kRiskDynamicBucket=2`、`kRiskExceptionFloor=2`、`kRiskEffectiveInstructionFloor=200`、code size bucket 边界 `<50/50-199/200-499/>=500`、synthetic 文件名片段 `{"<", "generated", "/_generated", "/genshi/", "/mako/", "/jinja", "/django/template/"}`（`isSyntheticFilename` 用）。这些常量已由 2026-06-02 Phase 0 C++ gate-side dump 通过 Mixed/family 红线，可作为 v1 coding/experiment defaults 进入代码和实验；它们不是生产 policy/default 冻结结论。
- **冻结契约补全（审校 scope-guardian/adversarial / Open Question 决策 5）**：risk/synthetic/active-dim 阈值喂入 key-bearing 字段（`risk_reason`、`code_size_bucket`、`is_synthetic`、`active_dim_mask`），因此与 opcode 表、cutoff/floor/δ/loop 一样属于 T3.11 进程内冻结的分类 schema——`skey_word` valid 后不得运行期变化，否则破坏“同一 code object 恒定身份”不变量（R20）。coding/experiment defaults 可被实现内置并通过 env/config 覆盖；`auto[:N]` 在生产默认冻结前保持 opt-in。正式生产默认值需经 `auto[:N]` vs 数值 `N` A/B、相邻 cutoff/floor/δ/loop/risk 配置比较、mis-defer 守门和 provider A/B 后冻结；后续重新标定必须在新进程或清空 code objects 后进入 gate/cache/policy，并保留部署覆盖入口（T3.3/T3.9）。Phase 0 dump、policy log 与 A/B report 必须输出 `autojit_config_id = hash(python_minor, opcode_table_version, opcode_table_coverage, payload_layout_version, cutoff/floor/δ/loop, risk_thresholds, code_size_bucket_bounds, active_dim_mask_layout, synthetic_filename_set, kDeferThresholdFactor)`；该 id 不参与热路径读取和 `skey_word` 打包。

---

# 11 Phase-3 参考边界：特化观测与滞回（不属于 v1 实现单元）

> **⚠ v1 不实现（审校 T2.2 / Open Question 决策 2）：** T3.1(b) 最小策略不读 `specialization_band`。本节不是 v1 实现单元，不定义 v1 字段、函数、接口或可直接编码的 C++ 伪代码；它只记录 Phase-3 后续设计必须满足的边界。

## 11.1 参考目标

Phase-3 若恢复特化观测，目标是把解释器中“曾经特化过”的弱信号作为 gate 当次微调输入。该信号不能证明类型当前稳定，只能说明函数曾经热过或曾经单态；因此它不得进入 `structure_key`，不得进入统计聚合键，也不得单独决定提前编译高 deopt 风险函数。

## 11.2 后续设计边界

| 维度 | Phase-3 必须满足的边界 |
|---|---|
| 数据来源 | 只读 code object / 解释器已沉淀状态；不得修改字节码或聚合身份 |
| 信号语义 | 弱“特化存在性”，不是类型稳定性证明 |
| 输出形态 | 有限离散 band，例如 low/mid/high；不得把连续比例直接暴露给策略 |
| 阈值行为 | 只能作为本次 gate 的有限幅微调输入 |
| 聚合契约 | 绝不进入 `structure_key`、map key、profile key 或任何 pattern 统计键 |
| 滞回要求 | 进入/退出高 band 必须使用不同阈值，避免相邻 gate 抖动 |
| 验收 | 必须包含 AE10：单态预热后转多态，验证弱信号不会被误当作稳定性证明 |

## 11.3 v1 明确不做

- 不在 `CodeExtra` 增加 `spec_band` 字段。
- 不在 `Signature` 中增加 `specialized` / `specializable` 计数。
- 不声明 `readSpecializationBand`、`SpecBand`、`isSpecializableOpcode` 等接口。
- 不在 `scanCode` 中读取 `specializedOpcode()`。
- 不为特化观测增加热路径刷新逻辑。

## 11.4 Phase-3 重新设计入口

Phase-3 开始时应单独形成详细设计或在本节扩展为正式实现单元，再决定字段、接口、刷新频率和原子发布语义。届时必须重新评估 §13 的 `calls < global` 短路前提：若 Phase-3 策略可能把阈值降到 `global` 以下，当前“先判全局阈值再分类”的顺序需要重审。

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
    return StructureKey::unpack(w & kSkeyPayloadMask);
  }
  // 未命中：计算（纯函数）。FT 下多线程可并发到此，结果逐位相等 → 良性。
  auto k = deriveStructureKey(code);
  if (!k.has_value()) {
    return std::nullopt;                                      // 不可分类/unknown opcode → 回退
  }
  uint32_t payload = k->pack() | kSkeyValidBit;
  _Py_atomic_store_uint32_release(&ex->skey_word, payload);       // 单字 release 发布
  return *k;
}
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

`std::optional<StructureKey>`(单元 10) → 有值时 `pack()|VALID` → `skey_word`(release) →〔下次〕acquire → `unpack()` → 单元 13；`nullopt` 不发布，调用方回退全局阈值。

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
- `CodeExtra` 新增字段（v1 仅 `skey_word`）置于 `jit_builtins` 之后，避免改动 `union calls/next` 与既有偏移敏感代码；`PyMem_Calloc` 已零初始化 → `skey_word=0` 即未分类。v1 不加 `spec_band`、schema/version 位（T3.11）。
- 实现时确认目标构建可用的 32 位 acquire/release 原子 helper；若 `_Py_atomic_load_uint32_acquire/_store_uint32_release` 不存在，使用等价 32 位 acquire/release helper 或封装本特性专用 helper。

---

# 13 实现设计 5：jitVectorcall 集成与最小阈值策略 computeThreshold

> **审校决策与穿刺证据已回灌：** T3.1(b)/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11 默认策略不再是 no-op，而是对明确 `raise_threshold_candidate` 抬阈值削减 compile storm；当前切片启用 low_roi、risk-defer、import/setup 高成本非数值候选；不可分类 module/class body 只进 Phase 0 `InitCodeDiagnostic`；Phase 0 C++ clean summary 已通过 Mixed/family 红线，可冻结分类 schema/evidence，并支撑 bootstrap 值作为 coding/experiment defaults，但不能冻结生产 policy/default；生产默认冻结前 `auto[:N]` 保持 opt-in；`startup_phase` 来源不得来自 `jitVectorcall` 内 frame/code metadata import-stack 采样，CinderX-only import/setup wrapper 可作 opt-in provider 验证，生产默认仍需安全 provider；`high_risk` 由 `risk_reason != 0` 派生，`risk_reason/code_size_bucket/active_dim_mask` 进入 key；compute-dominant 只认 `NumericLoop` 或 `Mixed` top-2 含 `Compute`；synthetic 低 ROI 默认只覆盖无 loop、非 static、ReflectionMeta/Trivial；`Mixed` 记录 top-2 `mixed_shape`；剩余并列选族采用 benefit-first tie-break；`structure_key` 物理缓存固定为 32-bit `skey_word`，字符串仅作诊断展示；分类配置进程内冻结，`skey_word` 无运行期失效；T2.1 `AutoJitPolicy` 虚类降为自由函数 `computeThreshold`；T2.2 v1 不读特化观测；T2.3 AutoJIT 分类入口复用 `PYTHONJITAUTO=auto[:N]` 启用。

## 13.1 实现概述

在 `jitVectorcall`（`cinderx/Jit/pyjit.cpp:183`，阈值门 `:197`）注入一次分类 + 阈值计算；提供**最小有用策略**：low_roi、risk-defer、import/setup 高成本非数值候选抬阈值，其余族走现状或稳态 warmup 阈值。**AutoJIT 分类入口复用 `PYTHONJITAUTO`（T2.3）**：`=auto[:N]` 开分类、`=<N>` 回到现状固定阈值。CinderX-only provider 实验另用 `CINDERX_AUTOJIT_IMPORT_PROVIDER` / `CINDERX_AUTOJIT_SETUP_PROVIDER`，不改变 `PYTHONJITAUTO` 语义。对应功能项 4（R18、R20、R26、R27、KD2/KD8、T3.1b/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11）。`startup_phase` 缺失时 import/setup 分支自然不命中；不得退而使用 `module_initializing` 或 `early_window` 单独替代完整 provider。

## 13.2 关键算法与流程

**改造点（最小侵入）**：把原"取全局 `compile_after_n_calls`"替换为"分类 → `computeThreshold`（失败/开关关 回退全局）"。

```cpp
// 热路径只消费安全 provider 输出的 O(1) depth/bool；
// startup_phase 是策略合并位，import/setup 是诊断和 A/B 细分位。
struct GateContext {
  bool startup_phase;
  bool import_phase;
  bool setup_phase;
};

enum class BranchReason : uint8_t {
  None,
  LowRoi,
  StartupInit,
  RiskDefer,
  FallbackInvalid,
  RoiBackoff,  // v1.5：roi_recompile_floor 生效（单元 14），非 computeThreshold 输出
};

struct ThresholdDecision {
  uint32_t limit;
  BranchReason branch_reason;
};

struct AutoJitGateState {
  CodeExtra* extra;
  uint64_t calls;
  GateContext context;
};

AutoJitGateState readAutoJitGateState(BorrowedRef<PyCodeObject> code) {
  CodeExtra* ex = codeExtra(code);
  uint64_t calls = ex != nullptr ? Ci_code_extra_get_calls(ex) : 0;
  return {ex, calls, readGateContext()}; // GateContext 不入 StructureKey
}

// pyjit.cpp jitVectorcall —— 改造后核心片段
BorrowedRef<PyCodeObject> code{func->func_code};
AutoJitGateState state = readAutoJitGateState(code);           // 内部只做一次 codeExtra get
CodeExtra* ex = state.extra;
uint64_t calls = state.calls;

// AutoJIT 未启用（compile_after_n_calls 无值）：保持现状——不进入分类/短路，直接走 forced 编译路径。
// 对齐 pyjit.cpp:197 的 has_value() 语义；`value_or(sentinel)` 会把“无阈值=即时编译”误转成
// “interpret-forever”，故显式分流（审校 feasibility/adversarial）。
if (!getConfig().compile_after_n_calls.has_value()) {
  return forcedJitVectorcall(func_obj, stack, nargsf, kwnames);  // 无阈值=即时编译（现状）
}
uint32_t global = *getConfig().compile_after_n_calls;

// 短路（正收益前提，对齐功能设计 §8.7.3.1）：computeThreshold 关于 global 单调非降，恒有
// limit >= global，故 calls < global 必然走解释路径，对其分类是死功。仅对 calls >= global
// 的编译候选分类，把"被扫描函数数"从全部 gate 可达收敛到编译候选（Phase 0 ~416k→~30k），
// 使分类开销与它要优化的编译工作量同阶。与现状等价：auto 关时此判等同原 `calls<limit`(limit=global)。
// 前提是策略单调非降；若未来策略（含 Phase-3）可把阈值降到 global 以下，须重审此处顺序。
if (calls < global) {
  return getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);  // 解释（现状）
}

uint32_t limit;
BranchReason branch_reason = BranchReason::None;
if (!getConfig().auto_classify) {                        // PYTHONJITAUTO=数值 → 分类关，等价现状（T2.3）
  limit = global;
} else if (auto sk = getOrComputeStructureKey(code, ex); !sk.has_value()) {
  limit = global;                                        // 缓存不可用 → 回退（KD8/R26）
  branch_reason = BranchReason::FallbackInvalid;
} else {
  ThresholdDecision d = computeThreshold(*sk, state.context, global);  // 单元 13 最小策略（无 SpecBand，T2.2）
  limit = d.limit;
  branch_reason = d.branch_reason;
  recordAutoJitPolicyDecision(*sk, d, state);             // branch_reason/risk_reason/code_size_bucket/active_dim_mask 进入 A/B report
}

if (calls < limit) {
  return getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);  // 解释（现状）
}
return forcedJitVectorcall(func_obj, stack, nargsf, kwnames);                // 编译（现状）
```

**最小策略（T3.1b，自由函数 T2.1）**：

```cpp
// 只对明确 defer candidate 抬高阈值（晚编译/几乎不编译）；
// 其余族走现状全局阈值或稳态 warmup 阈值。倍数为 coding/experiment default。
// computeDominantHint 只保护 NumericLoop 或 Mixed top-2 含 Compute；
// active_dim_mask 中带一点 incidental Compute 不保护对象/控制/分发主族。
ThresholdDecision computeThreshold(const StructureKey& sk, const GateContext& ctx, uint32_t global) {
  bool startup_like_family =
      sk.family == Family::CallDispatcher ||
      sk.family == Family::ReflectionMeta ||
      sk.family == Family::ObjectManipulator ||
      sk.family == Family::BranchFSM;
  bool startup_like_mixed =
      sk.family == Family::Mixed &&
      mixedShapeAllIn(
          sk.mixed_shape,
          {WorkDim::Dynamic, WorkDim::Dispatch, WorkDim::Object, WorkDim::Control});
  bool high_cost_nonnumeric_import_candidate =
      getConfig().enable_startup_init_policy &&
      ctx.startup_phase &&
      !sk.is_static &&
      sk.family != Family::NumericLoop &&
      !sk.computeDominantHint() &&
      (sk.highRisk() || sk.code_size_bucket > 0);
  if (high_cost_nonnumeric_import_candidate) {
    return {
        saturating_mul(global, kStartupDeferThresholdFactor),
        sk.highRisk() ? BranchReason::RiskDefer : BranchReason::StartupInit};
  }

  bool steady_nonnumeric_warmup_candidate =
      !sk.is_static &&
      sk.loop_score == 0 &&
      (sk.is_suspendable || startup_like_family || startup_like_mixed);
  if (steady_nonnumeric_warmup_candidate) {
    if (sk.highRisk() || sk.code_size_bucket > 0) {
      return {
          saturating_mul(global, kStartupDeferThresholdFactor),
          sk.highRisk() ? BranchReason::RiskDefer : BranchReason::LowRoi};
    }
    if (sk.family == Family::ObjectManipulator) {
      return {global, BranchReason::None};
    }
    return {max(global, kSteadyNonnumericWarmupThreshold), BranchReason::LowRoi};
  }

  bool large_branch_warmup_candidate =
      !sk.is_static &&
      sk.loop_score > 0 &&
      sk.family == Family::BranchFSM &&
      (sk.highRisk() || sk.code_size_bucket >= 2);
  if (large_branch_warmup_candidate) {
    return {max(global, kSteadyNonnumericWarmupThreshold), BranchReason::LowRoi};
  }

  bool synthetic_low_roi_candidate =
      sk.is_synthetic &&
      sk.loop_score == 0 &&
      !sk.is_static &&
      (sk.family == Family::ReflectionMeta || sk.family == Family::Trivial);
  bool low_roi_candidate =
      sk.family == Family::Trivial || synthetic_low_roi_candidate; // synthetic 高 loop/static 不默认 defer(T3.6)
  if (low_roi_candidate) {
    return {saturating_mul(global, kDeferThresholdFactor), BranchReason::LowRoi};
  }
  return {global, BranchReason::None};
}
// 出现第二种策略时再提升为多态接口（YAGNI，T2.1）
```

## 13.3 行为模型

### 13.3.1 正常流程

分类成功且开关开 → `computeThreshold` 给 `{limit, branch_reason}`（low_roi / risk-defer / import-setup high-cost nonnumeric candidate 抬高；其余=全局或稳态 warmup 阈值）→ 记录决策原因 → 解释/编译二分不变。

### 13.3.2 异常流程

- `sk==nullopt`（缓存不可用）→ 回退全局阈值。
- 分类关（`PYTHONJITAUTO=<N>` 数值）→ 不取分类、直接用全局阈值，等价现状（A/B 对照/止血，T2.3）。
- `computeThreshold` 为纯函数、不抛异常进入 gate；`branch_reason` 仅用于日志、A/B 与 mis-defer，不改变语义。

## 13.4 数据模型

### 13.4.1 数据结构定义

`computeThreshold` 为自由函数（T2.1，无策略对象/单例），返回 `ThresholdDecision{limit, branch_reason}`；新增配置项 `config.auto_classify`（bool，由 `PYTHONJITAUTO=auto[:N]` 解析置真，T2.3）与 `config.enable_startup_init_policy`（provider 可用时置真）；`GateContext` 为当次 gate 上下文，不入 `structure_key` / 不聚合。当前实现保留 `startup_phase = import_phase || setup_phase` 作为策略合并位，`import_phase` 与 `setup_phase` 先用于 compile event、phase A/B 和后续策略分叉评估；`computeThreshold` 暂不按二者分别给阈值。`StartupSignalMask` 只属于 Phase 0 诊断 dump，用于比较 importlib/module initializing、安全 import 状态 provider、早期进程窗口等候选信号，早期进程窗口不得单独成为默认策略来源；`kDeferThresholdFactor` / `kStartupDeferThresholdFactor` / `kSteadyNonnumericWarmupThreshold` 为 coding/experiment defaults，生产默认冻结前不作为推荐默认值。`kNoAutoJit` 哨兵已废弃：§13.2 改为以 `compile_after_n_calls.has_value()` 显式分流（无阈值=即时编译，保持现状），不再用 `value_or(sentinel)`。

**Import signal provider 约束：**

先区分两件事：import 路径上的函数和执行点是已有的，给 AutoJIT 读取的 `startup_phase/import_depth` 状态不是已有的。生产实现不能把“已有 import 入口”误写成“已有 provider”；provider 是本特性需要新增的解释器状态。

| 层级 | 已有吗 | 生产实现怎么用 | 说明 |
|---|---:|---|---|
| `IMPORT_NAME` / `_PyEval_ImportName*` / `PyImport_ImportModuleLevelObject` / `import_find_and_load` / importlib `_load_unlocked` | 有 | 作为候选挂点或链路核对点 | 这些点说明解释器有确定性 import 执行域 |
| `__spec__._initializing` | 有 | 只进 Phase 0/Phase 0.5 对照 | 覆盖不足，不等价于当前线程在 import 执行域 |
| `early_window` | 有 | 只进 Phase 0 对照 | 时间窗口不是语义状态 |
| CinderX import wrapper provider | 有实验实现 | opt-in 验证 import 窗口 | 包装 `_find_and_load` 或 `builtins.__import__`，不改 CPython |
| CinderX setup wrapper provider | 有 `lib2to3_main` 实验实现 | opt-in 验证明确定义的 setup/main 窗口 | 包装 `lib2to3.main.main()`，覆盖 `2to3` refactor/setup 初始化 |
| 生产 `import_depth/startup_phase` provider | 没有默认冻结 | 生产默认前需要新增或冻结 | 推荐在 CPython/CinderX import 路径用 RAII/try-finally 风格维护轻量计数，或证明 CinderX-only provider 覆盖目标 |

```text
进入 import 执行域: import_depth++
  执行 importlib/module top-level/import 嵌套调用
  jitVectorcall: import_phase = (import_depth > 0)  # O(1) 读取
  jitVectorcall: startup_phase = import_phase || setup_phase
退出 import 执行域: import_depth--
```

- 禁止在 `jitVectorcall` 中遍历 Python frame stack 或读取上层 frame code metadata 来判断 import stack；该路径已由 gdb 定位为 SIGSEGV。
- provider 应在 import machinery C 入口、CinderX import wrapper、明确 setup wrapper 或其它安全点维护轻量状态，例如 thread-local import depth/counter、模块初始化安全标志或等价信号；候选挂点/核对链路包括 `IMPORT_NAME` 生成代码、`_PyEval_ImportName*`、`PyImport_ImportModuleLevelObject`、`import_find_and_load` 与 importlib `_load_unlocked`，实际以目标版本生成代码和 import/setup 链路为准。`readGateContext()` 只做 O(1) 读取。CinderX-only wrapper 可先验证收益；生产默认必须补覆盖率/误伤率/gdb 证据。
- `module_initializing` 可作为辅助诊断信号，但 clean summary 中只覆盖 795/30605 个 storm，不能单独冻结为 `startup_phase`。
- `early_window` 可作为辅助/兜底候选，但不得单独成为默认策略来源。
- provider 缺失时，`startup_phase=false` 且 import/setup 分支不命中；low_roi / risk-defer 分支仍可独立验证和发布。ImportInit/setup 收益声明必须等待 provider 证据通过。
- `import_phase/setup_phase` 先只作为诊断字段：compile event 默认 `phase` 可输出 `import`、`setup`、`import_setup`、`startup` 或 `steady`。生产阈值策略是否按 import/setup 分叉，必须由分阶段 A/B 证明；当前提前 import 分类冻结和粗扩 setup/main window 均未通过。
- provider 通过线：gdb 下 import/setup-time JIT smoke 与代表性 workload 正常退出；dump 对 gate-reachable startup/import/setup storm candidate 达到 compile-time 加权覆盖率 ≥80%，或覆盖 top-20 candidate 并逐项解释未覆盖原因；post-import steady-state 中 provider 误置 `startup_phase=true` 的 candidate 数量与 compile-time 加权占比均 ≤5%；`readGateContext()` 热路径保持 O(1)。
- provider A/B 使用三组：`PYTHONJITAUTO=N` 固定阈值、provider-only deferral（只按 provider 统一延迟 candidate，不使用行为 family/Mixed 细分）、完整 `PYTHONJITAUTO=auto[:N]`。完整策略必须证明相对 provider-only 仍有增量价值，否则 v1 收益口径收窄为 provider-only 可解决的部分。

### 13.4.2 数据流转

`StructureKey(family + mixed_shape + modifiers + risk_reason + code_size_bucket + active_dim_mask) + GateContext` → `computeThreshold` → `ThresholdDecision(limit + branch_reason)` → 与 `calls` 比较，并把 `branch_reason/risk_reason/code_size_bucket/active_dim_mask/code identity` 写入 policy log / A-B report。聚合统计（下游）按完整 `StructureKey` 落库（R18/T3.7）；`GateContext` 只影响当次阈值，不落库、不聚合。Phase 0 可输出 `StartupSignalMask` 诊断字段，但它不进入 `skey_word` 或聚合 key；接入热路径前必须把 provider 输出折叠为冻结后的 `startup_phase` bool。**v1 无 `SpecBand`（T2.2）。**

> 术语对应（审校 T4.7/T3.4）：需求 R18 的 `gate_view` 是**概念名**；v1 中它等于 `structure_key + gate_context`（特化观测 defer 后不含 `SpecBand`），不引入持久化 `gate_view` 结构体。

## 13.5 接口设计

### 13.5.1 内部接口设计

`jitVectorcall` 只依赖 `readAutoJitGateState` + `getOrComputeStructureKey` + `computeThreshold`。`computeThreshold` 是唯一阈值决策点，且必须返回 `branch_reason`；下游升级策略时替换其实现（或在此提升为接口），不触碰分类器与 gate。

### 13.5.2 内部接口定义

```cpp
// startup_phase 是 import/setup provider 合并后的热路径策略输入；
// import_phase/setup_phase 用于诊断和后续 A/B，不进入 StructureKey。
struct GateContext {
  bool startup_phase;
  bool import_phase;
  bool setup_phase;
};
struct AutoJitGateState {
  CodeExtra* extra;
  uint64_t calls;
  GateContext context;
};
enum class BranchReason : uint8_t { None, LowRoi, StartupInit, RiskDefer, FallbackInvalid, RoiBackoff };
struct ThresholdDecision { uint32_t limit; BranchReason branch_reason; };
ThresholdDecision computeThreshold(const StructureKey& sk, const GateContext& ctx, uint32_t global);  // 自由函数（T2.1）
AutoJitGateState readAutoJitGateState(BorrowedRef<PyCodeObject> code);
void recordAutoJitPolicyDecision(const StructureKey& sk, const ThresholdDecision& d, const AutoJitGateState& state);
// readGateContext() 只能 O(1) 读取 provider 状态；不得遍历 Python frame stack。
// AutoJIT 分类启用由 PYTHONJITAUTO 解析决定（T2.3）：
//   -X jit-auto(空 X-option) -> 现状阈值 1, auto_classify=false
//   N        -> config.compile_after_n_calls=N, config.auto_classify=false  （现状）
//   auto     -> auto_classify=true, 全局阈值取默认
//   auto:N   -> auto_classify=true, 全局阈值=N
// 其它显式阈值入口（PYTHONJITALL、compile_after_n_calls()、auto_jit()）均清 auto_classify=false；
// JIT 初始化后重放已有 compile_after_n_calls 只调度函数，不改变 auto_classify。
// 聚合契约（注释强约束）：任何 pattern 统计 key == StructureKey（v1 无 SpecBand）
```

## 13.6 代码实现要点

- 改造仅限 `jitVectorcall` 内"求 limit"一段；解释/编译两条返回路径完全沿用现状（`getInterpretedVectorcall`/`forcedJitVectorcall`），降低回归面。
- **复用 `PYTHONJITAUTO` 作为 AutoJIT 分类入口（T2.3）**：把其注册从 `void(int)`（当前 `pyjit.cpp:300` 以 `[](uint32_t val)` lambda 绑定 `void(int)` 重载，FlagProcessor 无 `void(uint32_t)` 重载）改为 `void(const std::string&)`（FlagProcessor 已有该重载，`jit_flag_processor.h:84`），解析 `auto[:N]` 设 `config.auto_classify` + base 阈值，数值仍走原路径。`=N`（数值）= 分类关、等价现状，即 A/B 对照/止血手段。provider 实验开关（如 `CINDERX_AUTOJIT_SETUP_PROVIDER`）不改变该入口语义。parser contract：

| 输入 | 解析结果 | 说明 |
|---|---|---|
| `-X jit-auto`（空 X-option） | `compile_after_n_calls=1`，`auto_classify=false` | 保留现状；FlagProcessor 只把空 X-option 视为 1，空 env 不等价于 1 |
| 十进制 `uint32_t N` | `compile_after_n_calls=N`，`auto_classify=false` | 数值路径逐函数等价现状 |
| `auto` | `compile_after_n_calls=默认值`，`auto_classify=true` | 开启分类 + 最小策略 |
| `auto:N` | `compile_after_n_calls=N`，`auto_classify=true` | 开启分类，base=N |
| malformed / negative / empty env / overflow | 记录 invalid，字段保持原值 | 不把错误输入静默转成自动分类或阈值 1 |

- 状态转换实现须集中到 helper，避免既有 setter 意外保留/清除分类状态：

| 入口 | `compile_after_n_calls` | `auto_classify` | 实现要求 |
|---|---|---|---|
| `PYTHONJITAUTO=auto[:N]` | 默认值或 N | true | parser 显式置真 |
| `PYTHONJITAUTO=<N>` / `-X jit-auto` 空值 | N 或 1 | false | 逐函数等价现状 |
| `PYTHONJITALL` | 0 | false | 保持 compile-all 语义，不启用分类 |
| Python API `compile_after_n_calls(calls)` | calls | false | 显式 setter 关闭分类 |
| Python API `auto_jit()` | 1000 | false | 保持现有 API 语义 |
| malformed / overflow / empty env | 保持原值 | 保持原值 | 不静默改变状态 |
| JIT 初始化后重放已有阈值并调度已有函数 | 保持原值 | 保持原值 | 内部重放路径不得调用“清分类” helper |

- **入口激活契约（R27，必须落到初始化顺序）：** parser/config helper 只负责把阈值与 `auto_classify` 写入配置，不等于已经启用 AutoJIT。`jit::initialize()` 完成 flag/env/X-option 处理后，只要 `getConfig().compile_after_n_calls.has_value()`，就必须先安装 frame evaluator，再调度已有函数；新定义函数后续也必须通过该 evaluator 进入 `jitVectorcall` 计数。

```cpp
int initializeAutoJitEntryAfterFlags() {
  if (!getConfig().compile_after_n_calls.has_value()) {
    return 0;
  }

  // 这是入口激活的关键动作。没有它，新定义函数仍走 CPython 默认 evaluator，
  // count_interpreted_calls 不增长，阈值门也不会触发。
  if (Ci_InitFrameEvalFunc() < 0) {
    return -1;
  }

  schedule_existing_functions_for_jit(*getConfig().compile_after_n_calls);
  return 0;
}
```

解释：`configureCompileAfterNCalls()`、`configureAutoJit()`、`configureCompileAll()` 这类 helper 只能改配置；frame evaluator 安装必须在统一初始化路径兜底。回归测试不能只断言 `compile_after_n_calls==N` 或 `auto_classify==true`，还要在初始化后新建函数，验证前 `N` 次累计解释调用、第 `N+1` 次触发 JIT。`auto[:N]` 测试函数必须有明确循环，使最小策略不会因为 low-ROI 后移而掩盖入口是否真正生效。

- **回归基线**：开关关时编译函数集合与现状逐函数 bit-for-bit 一致（CI 守门）；开关开时仅 `raise_threshold_candidate` 编译时机后移，可单独验证收益。policy log 必须记录 `branch_reason`；`RiskDefer` / import-setup 分支还必须能按 `risk_reason/code_size_bucket/active_dim_mask` 过滤。
- `kDeferThresholdFactor`、bucket cutoff/floor、Mixed δ、loop count score、risk 阈值、code size bucket 边界与 active dim mask 布局等常量集中为 coding/experiment defaults；`PYTHONJITAUTO=auto[:N]` 是显式启用路径，生产默认冻结前保持 opt-in。正式热路径推荐默认值按 T3.2/T3.3/T3.9/T3.11 标定协议（混合语料）冻结：必须比较 `auto[:N]` 与数值 `N`，至少比较一组相邻 cutoff/floor/δ/loop/risk 设置，并通过 mis-defer 守门和 provider A/B；冻结后进程内不可变。所有 policy log / A-B report 输出 `autojit_config_id`，不同 id 的结果只能并列展示，不得聚合求结论。
- `readGateContext()` 实现必须有 gdb/smoke 验证：import-time JIT smoke 在 gdb 下正常退出；不得复现 `unicodeAsStringNoError -> isImportFrame -> hasImportStack -> recordGate -> jitVectorcall` crash 链。

---

# 14 实现设计 6：负 ROI 动态反馈与退避（RoiBackoff，v1.5）

> 上游：需求 KD9/R28–R31/L6/AE14–AE16；功能设计 §8.8。默认关闭、独立于 `auto_classify`；P1/P2 前提核实与 gdb smoke 通过前不得默认开启。

## 14.1 实现概述

在既有 deopt 出口 `prepareForDeopt`（`cinderx/Jit/codegen/gen_asm.cpp:149`，作用域内已有 `code_runtime` 与 `deopt_meta.reason`，经 `CodeRuntime::code()`（`code_runtime.h:61`）回链 code object）按 `DeoptReason` mask 做 relaxed 计数；计数达到当轮预算时复用 `jit::uncompile`（`context.h:161`，OSR 先例 `osr.cpp:655`）收回编译入口、设置重编译下限并指数加价；超过轮次上限置 `CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT` 冷位冻结。gate 在 `computeThreshold` 之后以 `roi_recompile_floor` 作为 calls 域下限。退避状态全部存 `CodeExtra` 增量字段，不进 `skey_word` payload、不参与聚合。

## 14.2 关键算法与流程

```cpp
// CodeExtra 增量字段（code_extra.h；并发契约见 14.4）
uint32_t roi_deopt_count;     // relaxed 自增（饱和），退避时清零
uint32_t roi_ctl;             // bit31 frozen | bit30 pending | bits24..27 round | 其余保留
uint64_t roi_recompile_floor; // calls 域下限；退避线程在入口保护内写，gate/OSR relaxed 读

// deopt 出口（prepareForDeopt 帧重建完成后、返回前调用；
// deopt_meta.reason 与 is_instrumentation_deopt 已在作用域内）
void recordDeoptForRoi(BorrowedRef<PyCodeObject> code,
                       DeoptReason reason,
                       bool is_instrumentation) {
  if (!getConfig().roi_backoff_enabled) {
    return;                                            // 开关关：bit-for-bit 等价现状
  }
  if (is_instrumentation || reason == DeoptReason::kPeriodicTaskFailure) {
    return;                                            // 默认 mask：与函数 ROI 无关
  }
  CodeExtra* ex = codeExtra(code);
  if (ex == nullptr) {
    return;                                            // 安全跳过（AE16c）
  }
  uint32_t ctl = atomicLoadRelaxed(&ex->roi_ctl);
  if (ctl & kRoiFrozen) {
    return;                                            // 已冻结：停止计数
  }
  uint32_t n = atomicAddRelaxed(&ex->roi_deopt_count, 1) + 1;  // 饱和实现略
  if (n < (kRoiDeoptBudgetBase << roundOf(ctl))) {
    return;                                            // 绝大多数 deopt 到此为止
  }
  triggerRoiBackoff(code, ex, ctl);                    // 罕见慢路径
}

void triggerRoiBackoff(BorrowedRef<PyCodeObject> code, CodeExtra* ex, uint32_t ctl) {
  // CAS 抢占：并发 deopt 单线程胜出（KD8 同款良性竞态）
  if (!atomicCas(&ex->roi_ctl, ctl & ~kRoiPending, ctl | kRoiPending)) {
    return;
  }
  // P1 前提：uncompile 仅解除入口链接与 jit_compiled 缓存，不释放机器码（14.6 核实清单）
  // P2 前提：经 Context per-code funcs 注册表解除该 code 全部 function 入口
  uncompileFuncsOfCode(code);   // 复用 deoptFuncImpl/uncompileImpl 既有遍历，入口保护内执行
  uint32_t k = roundOf(ctl);
  uint64_t floor = Ci_code_extra_get_calls(ex) +
      saturatingMul(*getConfig().compile_after_n_calls,
                    uint64_t{kRoiRewarmFactor} << k);
  atomicStoreRelease(&ex->roi_recompile_floor, floor);
  atomicStoreRelaxed(&ex->roi_deopt_count, 0);
  if (k + 1 > kRoiBackoffMaxRounds) {
    Ci_code_extra_or_skey_release(ex, CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT);  // 冻结走既有冷位 fast path
    atomicStoreRelease(&ex->roi_ctl, kRoiFrozen);
    recordRoiEvent(RoiEvent::kFreeze, code, ex, k);
  } else {
    atomicStoreRelease(&ex->roi_ctl, makeRoiCtl(/*round=*/k + 1));
    recordRoiEvent(RoiEvent::kUncompile, code, ex, k);
  }
}

// gate 集成（嵌入 §13.2 片段，computeThreshold 决策之后；floor 为 calls 域 u64，
// 与 ThresholdDecision.limit(u32) 分开比较，不做窄化）
uint64_t floor = atomicLoadRelaxed(&ex->roi_recompile_floor);
if (calls < floor) {
  branch_reason = BranchReason::RoiBackoff;
  return getInterpretedVectorcall(func)(func_obj, stack, nargsf, kwnames);  // 重新预热
}

// OSR 集成（osrCompileBudgetCheck 内，封死循环风暴函数的 OSR 后门）
if (roiFrozen(code) || Ci_code_extra_get_calls(ex) < atomicLoadRelaxed(&ex->roi_recompile_floor)) {
  return false;
}
```

判定常量（coding/experiment defaults，进程内冻结契约同 T3.11；生产值由 A/B 冻结）：`kRoiDeoptBudgetBase=256`、`kRoiBackoffMaxRounds=2`、`kRoiRewarmFactor=64`；reason mask 默认排除 `kPeriodicTaskFailure` 与 instrumentation deopt。开关：独立 env（如 `CINDERX_AUTOJIT_ROI_BACKOFF=1`），不改变 `PYTHONJITAUTO` 语义。

## 14.3 行为模型

### 14.3.1 正常流程

编译态函数 deopt → reason 过滤 → 计数；达预算 → CAS 单胜出 → uncompile 全部关联 func → floor 指数加价、round+1、计数清零 → 函数回 gate 重新预热；`calls >= max(limit, floor)` 后重编译进入下一轮观察（预算已翻倍）；round 超上限 → 冷位冻结，进程内不再编译、不再计数。

### 14.3.2 异常流程

- `codeExtra` 为 `nullptr`（分配失败/缺失）→ 跳过计数，不崩（AE16c）。
- CAS 失败 → 另一线程已在处理本轮退避，直接返回。
- uncompile 失败/不可用 → 清 pending、保持编译态并记诊断事件；本轮不加价，等下轮预算再试。
- frozen 后的 deopt → 直接返回（函数已在解释执行，deopt 仅可能来自残留栈上激活）。
- 开关关闭 → `recordDeoptForRoi` 入口早退，deopt/gate 路径与现状 bit-for-bit 等价（V7）。

## 14.4 数据模型

| 字段 | 类型 | 并发契约 | 说明 |
|---|---|---|---|
| `roi_deopt_count` | u32 | relaxed 自增/清零，饱和 | 当轮 reason-mask 过滤后 deopt 数 |
| `roi_ctl` | u32 | CAS 迁移；frozen/round 经 release 发布 | bit31 frozen、bit30 pending、bits24..27 round |
| `roi_recompile_floor` | u64 | 写侧入口保护内 release；gate/OSR relaxed 读 | calls 域下限；0 = 无下限 |

内存：`CodeExtra` 约 +16B/code object（Phase 0 1 万 unique code 量级 ≈ 160KB）。冻结复用 `skey_word` 的 `DECIDED_COLD` 位，与分类冷位同语义（本进程不编译）；`structure_key` payload 不变，聚合身份不受退避影响（KD9/R18）。状态不持久化，重启即清。

## 14.5 接口设计

```cpp
// behavior_classifier.h / pyjit.cpp 内部接口
void recordDeoptForRoi(BorrowedRef<PyCodeObject> code, DeoptReason reason, bool is_instrumentation);
// gate/OSR 只读消费：roiRecompileFloor(ex)、roiFrozen(ex)
// BranchReason 新增 RoiBackoff（§13.2/§13.5 枚举已同步）

// AutoJitGateStats 新增计数（pyjit.cpp:93 既有模式）
std::atomic<uint64_t> roi_uncompile;   // 退避次数
std::atomic<uint64_t> roi_recompile;   // floor 后重编译次数
std::atomic<uint64_t> roi_frozen;      // 冻结次数

// compile-events JSONL 新事件（CINDERX_AUTOJIT_COMPILE_EVENTS_FILE 既有通道）
// {"event":"roi_uncompile"|"roi_freeze", "code":<identity>, "skey":<structure_key 解码>,
//  "round":k, "deopts":n, "reasons":{<reason>:n,...}, "calls":c, "floor":f}
```

事件必须携带 `autojit_config_id`（含 budget/rounds/rewarm/mask），不同配置产物不得合并比较；`auto_classify` 关闭时 `skey` 字段可为空，事件仍输出。

## 14.6 代码实现要点

- **挂点**：`prepareForDeopt`（gen_asm.cpp:149）在帧重建与 instrumentation 处理完成后、返回 `DeoptResult` 前调用 `recordDeoptForRoi`；`deopt_meta.reason` 与 `is_instrumentation_deopt` 已在作用域内，无需额外取数。
- **P1 核实清单（合入前必须完成）**：阅读 `Context::uncompile`/`uncompileImpl`（`pyjit.cpp:4229`）确认仅解除入口链接与 `jit_compiled` 缓存、`CompiledFunction` 机器码生命周期由 Context 持有——deopt 调用点处在该函数编译帧内，递归/FT 并发激活的栈上机器码不得被释放。若不变量不成立：降级为 pending 标志 + 安全点执行（候选：周期任务/eval-breaker 侧，或 gate 侧全局 pending 队列），并重新评估本节流程。
- **P2 核实清单**：复用 `deoptFuncImpl`（`pyjit.cpp:1671`）的 per-code funcs 注册表遍历（`:4064` 先例），确认 `uncompileFuncsOfCode` 覆盖共享同一 code 的全部 function 对象。
- **FT 契约**：uncompile 在既有 free-threaded entrypoint guard 下执行（`code_extra.h` 注释）；三个增量字段按 14.4 原子契约访问，禁止半初始化读取。
- **等价门**：开关关闭时 `recordDeoptForRoi` 入口早退（单分支）；CI bit-for-bit 对比 deopt/编译行为（V7）。
- **测试映射**：RuntimeTests 新增 RoiBackoff 单元（AE14 风暴退避/冻结、AE15 可恢复性、AE16 等价/并发/故障注入）；gdb smoke 覆盖 deopt 出口触发 uncompile 的完整路径。

---

# 15 DFX分析

## 15.1 可靠性分析

| 风险 | 设计对策 | 验证 |
|---|---|---|
| 半初始化/撕裂读 `structure_key` | **单字** release/acquire 发布（单元 12.2），无值/标志分离 | AE11 + TSan |
| 统计被切碎（Phase-3 band 入键） | 类型隔离 + 聚合契约 + §11 参考边界 / §13.5 评审守门 | AE8 + 代码评审 |
| Mixed 聚合过粗 | `mixed_shape` 记录 canonical top-2 工作维度组合，最多 15 种 | AE12 + Phase 0 分布 |
| import-stack 采样崩溃 | 禁止热路径 frame stack/code metadata 遍历；改用安全 import signal provider | gdb smoke 必须正常退出 |
| AutoJIT 入口只改配置、不装 frame evaluator | 初始化路径在 `compile_after_n_calls.has_value()` 时统一调用 `Ci_InitFrameEvalFunc()`，再 schedule 已有函数 | AE13 / `CmdLineTest.JITAuto*InstallsFrameEvaluator` |
| 新 opcode 漏归类 | 全量表 283/283 覆盖单测；运行时表外 opcode 返回 `nullopt` 并回退全局阈值 | 单元 9.6 单测 + unknown opcode fault injection |
| 误把"曾单态"当稳定（Phase-3） | band 弱语义 + 限幅 + 滞回 | AE10 |
| 分类器异常拖垮 gate | 纯计算无抛异常路径；失败→回退默认阈值 | AE11 + 故障注入 |
| deopt 出口 uncompile 触碰活跃机器码（v1.5） | P1 前提核实：`jit::uncompile` 仅解除链接、机器码由 Context 持有；不成立则降级 pending + 安全点（单元 14.6） | AE14 + gdb smoke |
| RoiBackoff 误伤净正收益函数（v1.5） | 保守预算 + 有限轮次可恢复 + mis-backoff 守门（需求 L6/AE15） | AE15 + on/off A/B |

## 15.2 异常处理设计

- **缓存不可用**：gate helper 取得的 `CodeExtra*` 为 `nullptr` 或 `getOrComputeStructureKey(code, ex)` 返回 `nullopt` 时，gate 回退 `config.compile_after_n_calls`（KD8(c)）。不崩、不读半态。
- **不可分类或 unknown opcode**：`isAutoJitClassifiable(code)==false` 或 `scanCode` 遇到 `OpcodeClass::Invalid` 时，`deriveStructureKey` 返回 `nullopt`，gate 回退全局阈值；不写 `skey_word` valid 位。
- **空/退化 code**：`scanCode` 对 `n_eff==0` 短路 → `Trivial`；`bucketize` 防除零。
- **backedge 为空**：`loop_score=0`。
- **开关关闭**：全路径等价现状，作为兜底回退手段。
- **provider 不可用**：`startup_phase=false` 且 import/setup 分支不命中，不得退而使用 `early_window` 单独判定 ImportInit。
- 全链路无 C++ 异常向 `jitVectorcall` 传播（gate 不设 try/catch）。
- **RoiBackoff 异常路径（v1.5）**：`codeExtra` 缺失→跳过计数；CAS 失败→他线程已处理；uncompile 失败→清 pending、保持编译态、记诊断事件；开关关→入口早退、bit-for-bit 等价（单元 14.3.2）。

## 15.3 性能分析

- 命中路径：一次 `codeExtra` 取 + 一次 acquire 读 + 一次策略调用（默认 O(1)）。相对现状 `countCalls` 已做的 `codeExtra` 取，仅多一次 acquire-load + 一次（默认内联）策略调用。
- 首次路径：**单次** O(n) 扫描（n=指令数，工作维度 + 后向边一遍完成，T1.3）+ 一次 release 发布。**不再有第二遍 backedge 扫描。** 特化计数随 Phase-3 恢复。
- **被扫描函数数有界（正收益前提）：** §13 gate 仅在 `calls >= global` 时才调 `getOrComputeStructureKey`（短路见 §13 伪代码），故首扫只发生在编译候选上、不覆盖全部 gate 可达函数（Phase 0 ~416k→~30k）。总分类开销 ≈ 被扫描函数数 × 单函数 O(n)，主导项是被扫描函数数而非单函数成本（loop_score 等修饰位为 O(n) 内的常数项，§10.2）；净收益论证模型见功能设计 §8.9.1。
- **RoiBackoff 开销（v1.5）**：计数仅在 deopt 慢路径（帧重建已是主要成本），relaxed +1 为零阶；gate 多一次 u64 relaxed load 且仅在 `calls >= global` 之后；退避动作本身罕见（预算 256 起步、逐轮翻倍）。编译态正常执行路径零新增指令。
- **ROI 证据必须拆静态成本、动态成本和动态收益：** 静态成本包括编译次数、累计编译耗时、JIT code size/code cache、首轮/启动期耗时；动态成本包括 OSR entry/frame state 迁移、guard miss/fallback/deopt、runtime helper、generator/coroutine suspend/resume/reify 等正式执行期代价；动态收益包括稳态吞吐、candidate 执行时间下降和 branch-ablation/microbench 代理。pyperformance `warmups=3` 可能遮住编译、首次进入和一次性 OSR 等静态/一次性成本，因此 A/B report 不能只给正式 values，必须同时给编译与动态成本计数。
- FT 良性重复：最多 O(线程数) 次首扫，概率低、每次 O(n)，可接受；如实测偏高改 `compare_exchange` 单发布。
- **验收标准（R21，审校 T4.6 补全）：**
  - (V0) 全量 opcode 表覆盖：测试从 CPython 3.14 `opcode.opmap`、`opcode._specialized_opmap`、CinderX `cinder_opcode_ids.h` 与 `EAGER_IMPORT_NAME/EXTENDED_OPCODE` 生成期望输入集，断言实现表与文档表均为 **seen=283、unique=283、missing=0、extra=0、duplicate=0**；并用 golden 样例覆盖 `LOAD_GLOBAL→Dynamic`、`CALL→Dispatch`、`TO_BOOL→Control`、`CACHE→Ignored`、`LOAD_CONST→Neutral` 等代表项。
  - (V1) 稳态命中路径：准入路径单次开销相对基线回归 **≤ 2%**（startup micro-bench，以 `compile_after_n_calls` 现状为基线）。
  - (V2) 启动期：compile-storm 场景（数千 code object 同期跨阈）首扫总耗时相对"被推迟的一次编译"占比 **< 5%**（实测，超标则评估 `compare_exchange` 或惰性分类）。
  - (V3) 等价性：`PYTHONJITAUTO=<N>` / `auto_classify=false` 下，编译函数集合与调用计数与现状**逐函数 bit-for-bit 一致**（CI 回归对比，作为分类/缓存基建零行为变更的硬门）；`PYTHONJITAUTO=auto[:N]` 下仅 `raise_threshold_candidate` 按 `computeThreshold` 后移。
  - (V3a) 入口激活：`PYTHONJITAUTO=2`、`PYTHONJITAUTO=auto:2`、`-X jit-auto=auto:2` 初始化后，新定义的带循环函数必须在前两次调用累计 `count_interpreted_calls==2` 且未编译，第三次调用后 `is_jit_compiled==true`；同时断言数值入口 `auto_classify=false`、`auto:2` 入口 `auto_classify=true`。这是 frame evaluator 安装契约的集成守门，配置字段测试不能替代。
  - (V4) Provider 安全性：包含 import/setup-time JIT smoke 与代表性 workload 的 gdb 运行必须正常退出；`startup_phase` provider 复跑 dump 后，startup/import/setup storm candidate 的 compile-time 加权覆盖率 ≥80%，或 top-20 candidate 全覆盖/逐项解释；post-import steady-state 中 provider 误置 `startup_phase=true` 的 candidate 数量与 compile-time 加权占比均 ≤5%；不得只以 `module_initializing` 或 `early_window` 单信号冻结。
  - (V5) opt-in 策略 A/B：`PYTHONJITAUTO=auto[:N]` 相对 `PYTHONJITAUTO=N` 必须减少 candidate 编译次数、编译总耗时和 JIT code size/code cache；非 candidate 的编译行为保持等价，启动/吞吐无显著回归。报告必须分列 startup/setup 与 steady 指标，按 `branch_reason + risk_reason + code_size_bucket + active_dim_mask` 输出后移样本，并单列 guard/deopt/helper/suspend/OSR 等动态成本指标；该门槛只证明 opt-in 策略可发布，不冻结生产默认值。
  - (V5a) 生产 policy/default freeze：bootstrap/coding defaults 可进入实现；生产默认冻结前 `auto[:N]` 保持 opt-in。冻结生产推荐默认值前必须至少比较一组相邻 cutoff/floor/δ/loop/risk 配置，并输出被后移 top call-count / top compile-time / top lost-dynamic-benefit candidate 的 saved static cost 与 lost dynamic benefit 对比；还必须满足 provider A/B。所有报告必须携带 `autojit_config_id`。
  - (V5b) provider A/B：provider 通过后，在 import/setup/dispatch 密集真实 workload 上三组对比：固定 `N`、provider-only deferral、完整 `auto[:N]`。完整策略必须单独证明 ImportInit / setup compile storm 削减，以及相对 provider-only 的分类器增量价值；未通过前不得把 startup/setup storm 削减写入 v1 收益结论。
  - (V6) mis-defer / ROI 守门：v1 不要求静态签名完整预测 ROI，但所有默认后移分支都必须按 `structure_key + branch_reason + risk_reason + code_size_bucket + active_dim_mask + code identity` 记录 baseline/auto 是否编译、调用次数、baseline compile time、auto compile time、JIT code size、code cache、candidate 执行时间或 branch-ablation/microbench 代理，并记录 guard/deopt/helper/suspend/OSR 等动态成本计数。risk-defer / suspend / dynamic / exception 分支缺这些动态成本计数时，不得发布该分支，只能作为实验 FYI。`saved_static_cost = baseline_compile_time + baseline_code_cache_cost - auto_compile_time - auto_code_cache_cost`（未编译视为省下全部 baseline 静态成本）；`lost_dynamic_benefit = max(0, runtime_auto - runtime_baseline)` 或等价候选级估计。每个默认后移分支的 top call-count、top compile-time 与 top lost-dynamic-benefit/runtime-regression 样本 aggregate 必须满足 saved > lost，且无单个 top candidate 出现未解释的明显净损失；否则默认禁用或按 `risk_reason` / `code_size_bucket` / family / `mixed_shape` / `active_dim_mask` 收窄。synthetic 高 loop/static/generated 与 risk-defer candidate 尤其需要单独证明，不能从总体 compile 次数下降直接推出正收益。
  - (V7) RoiBackoff 等价与守门（v1.5）：开关关闭时 deopt/编译行为与现状 bit-for-bit 一致（CI 硬门）；开启时 AE14（风暴退避/冻结）、AE15（可恢复性/守门样本无回归）、AE16（FT 并发/故障注入）全部通过，gdb smoke 覆盖 deopt 出口 uncompile 完整路径；on/off A/B 按 mis-backoff 协议（需求 Outstanding Questions）对负样本（`sqlalchemy_declarative`/`dask`/`deepcopy` 子集）报告 saved dynamic cost vs lost benefit，结果回灌证据表；事件与报告携带含 budget/rounds/rewarm/mask 的 `autojit_config_id`。

## 15.4 安全和韧性分析

- 攻击面：仅只读 code object 既有字节码与 flags，无外部输入解析、无新增 syscalls。
- 内存所有权：复用 CPython code-extra 机制（`PyMem_Calloc`/`PyMem_Free`），生命周期随 code object，无新增泄漏路径。
- 韧性：任意子部件失败（分配、缓存、策略）均退回"现状全局阈值"语义，特性可一键开关；最坏情形仅影响"何时编译"（性能），不影响编译产物正确性。
- 并发：所有跨线程字段经 release/acquire 或 relaxed 原子访问，遵守 `CodeExtra` 既有 FT 契约（`code_extra.h:26`）。

---

# 16 上游可信源对照（实现锚点）

| 实现点 | 锚点（已核实） |
|---|---|
| gate 注入 | `cinderx/Jit/pyjit.cpp:183` `jitVectorcall`（函数起点；阈值门在 `:197`）；`:101` `countCalls` |
| `PYTHONJITAUTO` 启用 | `cinderx/Jit/pyjit.cpp:300` `jit-auto`/`PYTHONJITAUTO` 注册；`cinderx/Jit/jit_flag_processor.h:84` `addOption` 的 `void(const std::string&)` 重载（支撑 `=auto[:N]` 解析，T2.3）；`cinderx/Jit/pyjit.cpp:3696` 附近的 `compile_after_n_calls` 初始化重放路径必须调用 `Ci_InitFrameEvalFunc()` 后再 schedule；`cinderx/Interpreter/interpreter_base.cpp:16` 是 frame evaluator 安装实现锚点 |
| `PYTHONJITAUTO` parser 兼容边界 | `cinderx/Jit/jit_flag_processor.cpp`：空 X-option 视为 1，空 env 不等价于 1；malformed/overflow 需保持字段原值 |
| 公有 opcode 取值 | `cinderx/Jit/bytecode.cpp:106` `BytecodeInstruction::opcode()`（已 unspecialize + 复合 `EXTENDED_OPCODE_FLAG`）；`:153` `specializedOpcode()` 仅供 Phase-3 特化判定参考。**勿用** `private` 的 `uninstrumentedOpcode()`（`bytecode.h:57` private） |
| 指令遍历 | `cinderx/Jit/bytecode.h:102/111/163` `BytecodeInstructionBlock`/`Iterator`/`begin` |
| SP opcode 编码 | `cinderx/Interpreter/3.14/cinder_opcode_ids.h` `EXTENDED_OPCODE_FLAG=0x200`，SP opcode = `(n\|flag)` ≥512（T1.1 依据） |
| 后向边 | `cinderx/Jit/osr.cpp:327` / `osr.h:159` `collectBackedgeTargetOffsets`（仅 target、去重、上限 16）——**不提供 `{source,target}`**，故 loop_score 在本扫描内就地收集端点（T1.3） |
| 缓存载体 | `cinderx/Common/code_extra.h:12` `CodeExtra`（既有 `calls` 用 `_Py_atomic_add_uint64`/`_load_uint64_relaxed`，非 release/acquire） |
| 发布范式 | release/acquire 范式取自 `cinderx/Jit/context.cpp:523` `_Py_atomic_store_ptr_release`（`jit_compiled` 指针发布）——**`code_extra.h` 本身只描述、不实现该范式**；新 `skey_word` 沿用此 jit_compiled 范式。32 位 acquire/release helper 名称需在目标构建中核对；若 `_Py_atomic_load_uint32_acquire`/`_store_uint32_release` 不存在，使用等价 helper 或封装本特性专用 helper |
| get-or-create | `cinderx/Common/code.cpp:185` `codeExtra` + `CriticalSectionGuard`（`:200`）；NULL-on-failure |
| SP/flags / 可达性 | `cinderx/Jit/hir/preload.cpp:449` `CI_CO_STATICALLY_COMPILED`；`pyjit.cpp:96` `required_code_flags`；`pyjit.cpp:1160/1199/4025` eligibility/compile 前均拒绝缺 flags code，支撑 `InitCodeDiagnostic` 不进 v1 gate |
| import signal provider 候选挂点 | `cinderx/Interpreter/3.14/Includes/generated_cases.c.h` 与 `3.15/Includes/generated_cases.c.h` 的 `IMPORT_NAME` / `IMPORT_FROM` 调用 import C 入口；`cinderx/Jit/jit_rt.cpp:1457` `JITRT_ImportName` 只覆盖 JIT 代码发起的 import，不能单独作为 provider；生产 provider 需在目标 CPython/CinderX import machinery（如 `PyImport_ImportModuleLevelObject` / `import_find_and_load` / importlib `_load_unlocked` 链路）新增轻量状态，实际挂点以目标版本链路核对为准 |
| 特化弱语义 | `cinderx/Interpreter/3.14/Includes/ceval_macros.h` `DEOPT_IF`/`backoff_counter` |
| RoiBackoff 观测点 | `cinderx/Jit/codegen/gen_asm.cpp:149` `prepareForDeopt`（统一 deopt 出口，作用域内有 `code_runtime`/`deopt_meta.reason`/`is_instrumentation_deopt`）；`cinderx/Jit/code_runtime.h:61` `CodeRuntime::code()`；`cinderx/Jit/deopt.h:86` `DeoptReason` 全集 |
| RoiBackoff 退避动作 | `cinderx/Jit/context.h:161` `jit::uncompile`；`cinderx/Jit/osr.cpp:655` OSR "uncompile 后重编译"先例；`cinderx/Jit/pyjit.cpp:1671` `deoptFuncImpl` / `:4229` `uncompileImpl`（P1/P2 核实入口）；`cinderx/Common/code_extra.h` `CI_CODE_EXTRA_SKEY_DECIDED_COLD_BIT` 冷位 |
| RoiBackoff 诊断 | `cinderx/Jit/pyjit.cpp:93` `AutoJitGateStats`（既有全局原子计数模式）；`CINDERX_AUTOJIT_COMPILE_EVENTS_FILE` 既有事件通道 |
| 上游需求/功能设计 | `docs/design/autojit-behavior-classification/【需求分析】AutoJIT 行为模式分类.md`；`docs/design/autojit-behavior-classification/【功能设计】AutoJIT 行为模式分类.md` |
| Phase 0 C++ evidence | `scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md`、`summary-clean/summary.json`、`logs/autojit-phase0-gdb-debug-container-20260602-115858.log`、`logs/autojit-phase0-gdb-after-fix-20260602-120011.log` |
