# 功能设计说明书 — 自适应 AutoJIT 行为模式分类

## 1 产品版本&密级

| 项 | 内容 |
|---|---|
| 产品 | CinderX JIT（**目标 Python 3.14+**，含 Static Python） |
| 特性 | 自适应 AutoJIT 行为模式分类（Behavior Pattern Classification） |
| 版本 | v0.2（草案） |
| 密级 | 内部公开 |
| 适用分支 | `codex/hot-loop-osr-lightweight-docs` 及后续 AutoJIT 演进分支 |

## 2 拟制信息

| 角色 | 信息 |
|---|---|
| 拟制 | CinderX 性能优化组 |
| 日期 | 2026-06-01 |
| 上游需求 | `docs/brainstorms/2026-05-31-autojit-behavior-classification-requirements.md` |
| 关联 Issue | sisibeloved/cinderx#3《探索基于行为模式的自适应 AutoJIT 阈值策略》 |
| 评审 | 待评审（已过一轮 Codex 对抗式审查，三项 high/medium 已闭环） |

## 3 修订记录

| 版本 | 日期 | 修订人 | 修订说明 |
|---|---|---|---|
| v0.1 | 2026-06-01 | 性能优化组 | 首版。依据需求文档 R1–R26 与源码实测，拆分 4 个功能项，定义逻辑接口、调用路径与 DFX。 |
| v0.2 | 2026-06-02 | 性能优化组 | 根据 Phase 0 C++ dump 与 gdb 定位更新证据边界：冻结 gate-side 分类 schema/Mixed 红线，禁止 `jitVectorcall` 内 frame/code metadata import-stack 采样，并将 `startup_phase` 安全来源列为待决。 |

## 4 Keywords 关键词

AutoJIT、行为签名、structure_key、特化观测、编译准入阈值、free-threaded 发布、osr backedge、Static Python、compile storm。

## 5 Abstract 摘要

本功能设计将"按行为模式区分 AutoJIT 编译阈值"的需求落到模块级实现方案。核心是一个**行为分类器模块**：在 AutoJIT 准入点（`jitVectorcall`）对每个 gate 可达 code object 做一次廉价的字节码扫描，派生出**确定性的稳定聚合身份 `structure_key`**（族 + Mixed-only `mixed_shape` + 结构修饰位），缓存进 `CodeExtra` 并遵守 free-threaded 发布契约；再由自由函数 `computeThreshold(structure_key, gate_context, global)` 给出本次阈值。**v1 交付 = 分类法 + 最小有用策略**：分类侧记录 `mixed_shape`、采用 benefit-first tie-break，并用 bootstrap cutoff/floor/loop defaults 启动 Phase 0 dump（T3.7/T3.8/T3.9）；缓存侧固定 32-bit `skey_word` 且配置进程内冻结、无运行期失效（T3.10/T3.11）；策略侧对明确 `raise_threshold_candidate` 抬阈值削减 compile storm（T3.1b/T3.4/T3.5/T3.6）。2026-06-02 Phase 0 C++ clean summary 已冻结 gate-side schema 与 Mixed/family 红线（53 个 dump 进程、417389 records、30605 storm、Mixed(storm)=2.9%），但 gdb 证明 `jitVectorcall` 内 frame/code metadata import-stack 采样不安全；因此 `gate_context.startup_phase` 不能由原始 `import_stack` 遍历或单独 `module_initializing` 冻结，必须等待安全 import signal provider。启用通过 `PYTHONJITAUTO=auto[:N]`，设回数值即回退（不新增环境变量，T2.3）。`high_risk` 保留为成本/安全 modifier，不再一刀切视为低 ROI；synthetic/generated 单独成为 `is_synthetic` modifier。特化观测、完整阈值映射、在线反馈为下游（v1 不实现，T2.2）。设计严格区分"稳定聚合身份"与"当次 gate 上下文"避免统计被切碎，并对并发发布、回退路径给出明确契约。

## 6 List of abbreviations 缩略语清单

| 缩略语 | 英文全称 | 中文名 |
|---|---|---|
| JIT | Just-In-Time compilation | 即时编译 |
| OSR | On-Stack Replacement | 栈上替换 |
| HIR | High-level Intermediate Representation | 高层中间表示 |
| ROI | Return On Investment | 收益成本比 |
| AE | Acceptance Example | 验收示例 |
| FT | Free-Threaded (Python) | 自由线程 Python |
| CFG | Control Flow Graph | 控制流图 |
| SP | Static Python | 静态 Python |
| DFX | Design for X | 面向 X 的设计 |
| FMEA | Failure Mode and Effects Analysis | 失效模式与影响分析 |
| PGO | Profile-Guided Optimization | 基于剖析的优化 |
| SR | System Requirement | 系统需求 |

## 7 前言

本文档是上游需求文档（`docs/brainstorms/2026-05-31-autojit-behavior-classification-requirements.md`，下称"需求文档"，其条目以 R1–R26、KD1–KD8、AE1–AE12 引用）的功能设计落地。读者对象为 CinderX JIT 开发者与评审。本文档聚焦**模块对功能的实现方式、模块间逻辑接口与调用路径**，以语言无关伪代码描述新增逻辑；对既有 C/C++ 结构（如 `CodeExtra`、`jitVectorcall`、`BytecodeInstruction`）仅作为集成边界引用，不重述其实现。具体到某语言/运行环境的内部实现留待详细设计。第一可信源为项目代码，关键设计点均标注已核实的源码位置。

---

# 8 功能域：自适应 AutoJIT 行为模式分类

## 8.1 功能域概述

当前 AutoJIT 用单一全局阈值 `compile_after_n_calls` 决定何时编译（已核实 `cinderx/Jit/pyjit.cpp:183` `jitVectorcall`，阈值门 `:197`：`countCalls(code) < limit` 则解释，否则编译）。低阈值在启动期造成 compile storm，编译开销泄漏进 benchmark 与短生命周期 worker。

本功能域引入一个**行为分类器**，把"所有函数共享一个阈值"升级为"按函数行为模式区分阈值的依据"。v1 交付两个闭环能力：
- 给每个 code object 产出一个**稳定、确定、有界**的聚合身份 `structure_key`（族 + Mixed-only `mixed_shape` + 结构修饰位）；
- 在准入点通过自由函数 `computeThreshold(structure_key, gate_context, global)` 提供**最小有用策略**（明确 `raise_threshold_candidate` 抬阈值，其余走现状）。

弱特化观测、完整阈值映射以及 pattern 级在线反馈属下游功能域；本设计仅保留 Phase-3 参考接口，v1 不实现、不读取 `specialization_band`。

## 8.2 功能域总体方案

### 8.2.1 模块划分

新增单一模块 **行为分类器（BehaviorClassifier）**，v1 内部分解为 3 个协作子部件；另保留 Phase-3 特化观测参考项：

| 子部件 | 功能项 | 职责 |
|---|---|---|
| 签名扫描与派生（SignatureScanner + KeyDeriver） | 功能项 1（8.4） | 单次字节码扫描 → 6 工作维度 + loop_score + 结构修饰位 → 预过滤/argmax → `structure_key` |
| 特化观测旁路（SpecializationObserver） | Phase-3 参考（8.5） | v1 不实现；后续旁路统计已特化 opcode 比例 → 带滞回的 specialization_band（弱信号，不进 key） |
| 结构身份缓存（StructureKeyCache） | 功能项 3（8.6） | 把 `structure_key` 发布/读取于 `CodeExtra`，遵守 FT release/acquire 契约与失败回退 |
| 准入点集成（PolicyGate） | 功能项 4（8.7） | 在 `jitVectorcall` 注入分类 + `computeThreshold(structure_key, gate_context, global)` 最小策略（T3.1b/T3.4/T3.5/T3.6/T2.1）；v1 不读特化观测 |

### 8.2.2 模块级 4+1 视图

**逻辑视图（Logical）**

```
                 ┌───────────────────────────────────────────────┐
                 │            BehaviorClassifier 模块             │
                 │                                               │
   PyCodeObject ─┤  SignatureScanner ─► KeyDeriver ─► structure_key
                 │        │  (功能项1)                            │
                 │        └─► SpecializationObserver ─► spec_band │  ← Phase-3 才加(T2.2)
                 │                 (功能项2, v1 不实现)           │
                 │                                               │
                 │  StructureKeyCache (功能项3, CodeExtra 发布)   │
                 └───────────────┬───────────────────────────────┘
                                 │ 逻辑接口 (功能项4)
                                 ▼
                 computeThreshold(structure_key, gate_context, global)
                   ← v1 最小策略(T3.1b/T3.4/T3.5/T3.6/T2.1)
                   low_roi / startup-init / risk-defer candidate 抬阈值；其余现状
```

**进程视图（Process）**：分类在 AutoJIT 准入路径（`jitVectorcall`）同步触发，处于解释执行线程上下文。`structure_key` 每 code object 仅计算一次（缓存命中后零成本）；FT 构建下多线程可并发首次计算，竞态良性（纯函数，逐位相等）。无独立后台线程。

**开发视图（Development）**：新增 `cinderx/Jit/behavior_classifier.{h,cpp}`（分类器主体）；依赖既有 `cinderx/Jit/bytecode.*`（opcode 遍历与归一）、`cinderx/Jit/osr.*`（后向边 opcode 语义参照；端点在分类器扫描内收集）、`cinderx/Common/code_extra.h`（缓存载体，需扩展）、`cinderx/Common/code.cpp`（`codeExtra` get-or-create）。被 `cinderx/Jit/pyjit.cpp`（`jitVectorcall`）调用。

**物理/部署视图（Physical）**：进程内特性，无网络/分布式形态。状态为每 code object 的 `CodeExtra` 扩展字段（堆内存，随 code object 生命周期，由 CPython code-extra 机制 `PyMem_Free` 回收）。**不涉及**跨进程/跨主机部署。

**场景视图（Scenarios）**：以需求文档 AE1–AE9、AE11–AE12 为 v1 关键场景——数值嵌套循环、async 协程、SP 类型化函数、分发器、模块体、薄 getter、synthetic 代码、`structure_key` 确定性回归、正交性回归、FT 并发首次分类、Mixed top-2 形态保真。AE10 特化多态回归随 Phase-3 特化观测实现。

### 8.2.3 总体调用路径变更（功能域级）

**变更前（现状）**

```
jitVectorcall(func)                                  # pyjit.cpp:183，阈值门 :197
  ├─ limit = config.compile_after_n_calls
  ├─ calls = countCalls(code)                        # pyjit.cpp:101
  └─ calls < limit ? 解释(getInterpretedVectorcall) : 编译(forcedJitVectorcall)
```

**变更后（本功能域）**

```
jitVectorcall(func)
  ├─ extra = codeExtra(code)                                    # gate helper 统一获取一次
  ├─ calls = extra ? getCalls(extra) : 0
  ├─ sk   = BehaviorClassifier.getOrComputeStructureKey(code, extra) # 功能项1+3
  │         └─ 命中缓存→直接返回；未命中→扫描派生并发布（失败→返回"无效"）
  ├─ ctx   = gate_context                                       # startup_phase 等，不入 sk
  ├─ limit = computeThreshold(sk, ctx, global)                  # 功能项4：最小策略（T3.1b/T3.4/T3.5/T3.6/T2.1）
  │         └─ sk 无效 或 开关关 → limit = config.compile_after_n_calls（回退，KD8/R26）
  │         └─ low_roi / startup-init / risk-defer candidate→抬高阈值；其余→现状
  └─ calls < limit ? 解释 : 编译
       # 下游统计一律按 sk 聚合
       # 特化观测 band：v1 不做，Phase-3 才加（T2.2）
```

## 8.3 功能域规格设计

| 规格项 | 规格 | 来源 |
|---|---|---|
| 聚合身份有界性 | family ≤ 8；`Mixed` 子形态 ≤ 15；Phase 0 C++ clean summary 已验证 `Mixed(storm)=2.9%`、最大 storm family 未超过 50% | R3/R15/R16 |
| 确定性 | 同一 code object 的 `structure_key` 逐位恒定，不随运行漂移 | R20/AE8 |
| 穷尽性 | 每个 gate 可达 code object 恰好一个族（含 Trivial/Mixed 兜底）；不可达初始化代码进诊断桶 | R19/T3.4 |
| 廉价性 | 单次 O(n) 字节码扫描，每 code object 一次；准入路径不得成为新热点 | R21 |
| 版本鲁棒 | 以 opcode 家族表匹配，覆盖 3.14 基础 + Static Python 两类（目标 3.14+，未来 minor 扩表）| R23/KD5 |
| 并发安全 | FT 构建下发布/读取遵守 release/acquire；失败回退默认阈值 | R26/KD8 |
| 特化信号弱语义（Phase-3） | specialization_band 仅次级微调，不进 key、不聚合，带滞回 | R11/R18/R20/KD6 |

---

## 8.4 功能项 1：行为签名提取与 structure_key 派生

### 8.4.1 功能概述

对一个 gate 可达 code object 做单次字节码线性扫描，按"opcode 家族 → 工作维度"归类计数，结合 `co_flags`/loop 结构与 OSR backedge，派生出确定性的 `structure_key`（族 + Mixed-only `mixed_shape` + 结构修饰位）。Phase 0 诊断 scanner 额外识别不可达 module/class body 为 `InitCodeDiagnostic`，但该诊断桶不属于 v1 `structure_key`。覆盖需求 R1–R10、R12–R20、R22–R25。

### 8.4.2 实现思路

1. **归一遍历**：用既有**公有** `BytecodeInstruction::opcode()`（`cinderx/Jit/bytecode.cpp:106`，已 unspecialize 且对 SP 复合 `EXTENDED_OPCODE_FLAG`）取 canonical opcode，再查"家族表"归入唯一一个工作维度（R22/R25，每 opcode 唯一归属）。**勿用** `private` 的 `uninstrumentedOpcode()`（会漏 SP opcode，审校 T1.2）。
2. **6 工作维度**：compute / control / object / dispatch / suspend / dynamic（R1–R6）。同次遍历**就地收集** loop 结构所需的后向边端点（既有 `collectBackedgeTargetOffsets` 仅返回 target、无 source，故在本扫描内由 `getJumpTarget()` + 当前 index 就地记录，审校 T1.3）与异常子计数（供 risk 派生）。
3. **结构修饰位**：`loop_score`(0–3，由后向边嵌套深度 + backedge 数 bootstrap 映射，R7/T3.9)、`is_static`(由 `CI_CO_STATICALLY_COMPILED`，已核实 `preload.cpp:449`，R9)、`is_suspendable`(由 `co_flags` 生成器/协程位，R10)、`high_risk`(由已分配计数派生，不重复计 opcode，R8/KD7)、`is_synthetic`(由稳定元数据派生，T3.6)。
4. **可达性分流 + 预过滤**：Phase 0 诊断 scanner 将缺 `CO_OPTIMIZED|CO_NEWLOCALS` 或 `co_name=="<module>"` 标为 `InitCodeDiagnostic`（R12/T3.4），不生成 v1 `structure_key`；gate 可达路径只保留全维度低于 floor → `Trivial`（R13）。
5. **分桶 + 选族**：density 分桶（计数/有效指令数，带 floor，R14）；近并列 → `Mixed` 并记录 canonical top-2 `mixed_shape`（R16/T3.7）；未落 `Mixed` 的剩余并列用 benefit-first tie-break `compute > dispatch > object > control > dynamic > suspend`（R15/T3.8）。compute 主导一律归 `NumericLoop`，由 `loop_score` modifier 区分有无循环（审校 T2.4，原 `ScalarCompute` 并入为 `NumericLoop|loop0`）。
6. **派生 key**：`structure_key = family | mixed_shape | {结构修饰位}`（R18），编码为紧凑整型（详细设计定）。

### 8.4.3 实现设计

#### 8.4.3.1 opcode 家族表设计

维护 `family_table: canonical_opcode → work_dimension`（R23），**只覆盖 3.14 的两类 opcode**（目标 3.14+，KD5）：(1) **3.14 基础**（`BINARY_OP`/`CALL`/`SEND`/`JUMP_BACKWARD`/`TO_BOOL`/`BUILD_TEMPLATE`…，<256）；(2) **Static Python**（`(n|EXTENDED_OPCODE_FLAG)` ≥512：`PRIMITIVE_*`→compute、`INVOKE_*`→dispatch、`SEQUENCE_*`/`STORE_FIELD`→object、`CAST`/`CONVERT_PRIMITIVE`→compute，R24）。旧式 3.10/3.11 opcode（`BINARY_ADD`/`CALL_FUNCTION`/`JUMP_ABSOLUTE`）在 3.14 不存在，不入表。表外 opcode（控制/栈管理无语义贡献者）归入"中性"，不计入任何工作维度但计入"有效指令总数"分母（除 `EXTENDED_ARG`/`EXTENDED_OPCODE`，作非语义跳过）。

#### 8.4.3.2 loop_score 派生设计

在分类器自身的单次扫描内就地收集后向边端点 `(source, getJumpTarget())`（既有 `collectBackedgeTargetOffsets`（`osr.cpp:327`）仅返回 target、上限 16，不含 source，故不复用，审校 T1.3）；按后向边的静态区间包含关系计算最大嵌套深度，并结合 backedge 数得到 bootstrap `loop_score = max(nesting_score, count_score)`：backedge 数 `0/1/2–3/>=4` 映射 `0/1/2/3`（R7/T3.9）。本扫描只读结构，不触发 OSR 编译；该映射进入 gate/cache/policy 前须由 Phase 0 dump 冻结或调整。

#### 8.4.3.3 派生流水线设计

```
派生流水线（伪代码，语言无关）
function deriveStructureKey(code):
    assert hasRequiredFlags(code) and code.name != "<module>"                # 不可达 init code 只进 Phase 0 诊断桶
    sig = scan(code)                       # 见 8.4.3.4，单次遍历
    mods = structuralModifiers(code, sig)  # loop_score,is_static,is_suspendable,high_risk,is_synthetic
    if allBelowFloor(sig.workdims):
        return KEY(family=Trivial, modifiers=mods)                           # R13
    buckets = bucketize(sig.workdims)                                        # R14
    (top1, top2) = topTwo(buckets, benefitFirstTieBreak)                     # R15/T3.8
    if top1 - top2 <= MIXED_BUCKET_DELTA and bothAtLeast(top1, top2, MIXED_MIN_BUCKET):
        return KEY(family=Mixed,
                   mixed_shape=canonicalPair(top1.dim, top2.dim),
                   modifiers=mods)                                           # R16/T3.7
    family = mapDominant(top1.dim, mods.loop_score)                           # R15 表
    return KEY(family=family, modifiers=mods)
```

#### 8.4.3.4 单次遍历设计

```
function scan(code):
    counts = zeroed(workdims); exc = 0; backward_edges = []   # 就地收集，非第二遍
    n_eff = 0
    for instr in instructions(code):           # 跳过 EXTENDED_ARG/EXTENDED_OPCODE
        op = opcode(instr)                      # 公有 canonical opcode（含 SP flag），R22 归一
        # （Phase-3 才加 observeSpecialization：specializedOpcode()!=opcode()，T2.2 v1 不做）
        dim = family_table[op]                  # 唯一归属（R25）
        if dim != NEUTRAL: counts[dim] += 1
        if isExceptionOpcode(op): exc += 1      # 供 risk 派生
        if isBackwardJump(op):                  # 就地记录后向边端点（T1.3）
            tgt = getJumpTarget(instr)
            if tgt < index(instr): backward_edges.append((index(instr), tgt))
        n_eff += 1
    return Signature(counts, exc, n_eff, backward_edges)
```

### 8.4.4 增量SR清单

| SR 编号 | 描述 | 对应需求 |
|---|---|---|
| SR-CLS-01 | 提供 `deriveStructureKey(code)`，对任意 code object 产出唯一族 | R15/R19 |
| SR-CLS-02 | 维护覆盖 3.14 基础 + Static Python 两类 opcode 的家族表，按家族匹配 | R23/R24/KD5 |
| SR-CLS-03 | 由 OSR backedge 派生 `loop_score`（0–3） | R7 |
| SR-CLS-04 | 由 `co_flags`/`CI_CO_STATICALLY_COMPILED` 派生结构修饰位 | R9/R10 |
| SR-CLS-05 | risk 由已分配计数派生，不重复计 opcode | R8/R25 |
| SR-CLS-06 | density 分桶带 floor，argmax 选族带 benefit-first tie-break/Mixed | R14/R15/R16 |

### 8.4.5 实现接口设计

#### 8.4.5.1 实现接口设计（说明）

功能项 1 对外暴露一个纯函数式接口 `deriveStructureKey`，输入只读 code object，输出值类型 `StructureKey`。无副作用（特化观测旁路写入由功能项 2 承接，且只读特化态）。内部家族表为编译期常量。

#### 8.4.5.2 实现接口定义（逻辑接口，语言无关）

```
type WorkDim   = enum { compute, control, object, dispatch, suspend, dynamic }
type Family    = enum { NumericLoop, BranchFSM, ObjectManipulator,    // T2.4: 去 ScalarCompute
                        CallDispatcher, AsyncStateMachine, ReflectionMeta,
                        Trivial, Mixed }                              // 共 8 个
type MixedShape = enum { none, pair(compute,control), ... }            // 仅 Mixed 使用；canonical unordered pair，≤15
type Modifiers = record { loop_score: 0..3, is_suspendable: bool,
                          is_static: bool, high_risk: bool,
                          is_synthetic: bool }
type StructureKey = record { family: Family, mixed_shape: MixedShape,
                             modifiers: Modifiers }                   # 即聚合身份
type GateContext  = record { startup_phase: bool }                    # 安全 import signal provider 冻结来源；不入 key / 不聚合

interface deriveStructureKey(code: ReadOnlyCode) -> StructureKey      # 确定、纯函数
```

### 8.4.6 功能规格设计

- 单次扫描复杂度 O(指令数)；不分配大对象（计数器为栈上小数组）。
- 确定性：对同一 code object 的字节码 + `co_flags`，输出恒定（R20）。
- 穷尽：返回族集合覆盖全部输入，无"未知"（R19）。

### 8.4.7 DFX分析

#### 8.4.7.1 可靠性分析

##### FMEA分析

| 失效模式 | 原因 | 影响 | 缓解 |
|---|---|---|---|
| 新 opcode 漏归类 | 版本升级新增 opcode 未入表 | 落入 NEUTRAL，密度被低估 | 家族表按家族匹配 + 单元测试对全 opcode 集合断言有归属；未知 opcode 计入分母但不致崩溃 |
| 选族抖动 | 维度近并列 | 同类函数分到不同族 | Mixed 兜底（R16）+ canonical top-2 `mixed_shape`（T3.7）+ benefit-first 固定 tie-break（T3.8）；正交性规则（R25）减小相关维度耦合 |
| loop 嵌套误判 | 非规约后向边/异常跳转 | loop_score 偏高/偏低 | 仅用 `BytecodeInstruction::getJumpTarget()` 覆盖的后向跳转 opcode（`JUMP_BACKWARD` / `JUMP_BACKWARD_NO_INTERRUPT` / `*_JIT` / `*_NO_JIT`），异常跳转归 control 不计 loop |

#### 8.4.7.2 可服务性分析

提供 Phase 0 诊断输出：先落一条不接入 policy 的 scanner/dump 路径，对给定 code object dump `(workdim counts, buckets, family, mixed_shape, modifiers, structure_key, gate_context, startup_signal_mask, diagnostic_bucket)`，便于分布采样与标定。Phase 0 scanner 可用 bootstrap defaults 起跑：`count_floor=2`、density cutoff `0.10/0.25/0.50`、`MIXED_BUCKET_DELTA=1`、`MIXED_MIN_BUCKET=2`、`loop_score=max(nesting_score,count_score)`。2026-06-02 C++ gate-side dump（`scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md`）已通过 schema 红线：`process_count=53`、`observed_records=417389`、`gate_reachable=416381`、`storm_candidates_reached_threshold=30605`、`compiled_records=23446`、`Mixed(storm)=2.9%`。因此这些 bootstrap defaults 可作为 v1 编码默认起点；进入 gate/cache/policy 后分类 schema/config 仍按 T3.11 进程内不可变，已缓存 `skey_word` 不失效。

`diagnostic_bucket=InitCodeDiagnostic` 用于缺 required flags / `<module>` 的不可达初始化代码；`mixed_shape` 用于解释 `Mixed` 分布是否集中在少数 top-2 组合；`startup_signal_mask` 同时记录 importlib/module initializing、安全 import 状态 provider、早期进程窗口等候选信号。注意：Phase 0 C++ gdb 证据禁止在 `jitVectorcall` 内遍历 Python frame/code metadata 来采样 `import_stack`；`module_initializing` 在 clean summary 中只覆盖 795/30605 个 storm，不能单独作为 `startup_phase`；`early_window` 只能作辅助/对照信号。`gate_context.startup_phase` 必须等安全 import signal provider 复跑通过覆盖率/误伤率后，才冻结为热路径 bool。建议复用既有 JIT 日志开关风格（如 `PYTHONJITDUMPHIRSTATS` 的形态）。

#### 8.4.7.3 安全设计检查

##### 安全设计确认

仅读取 code object 既有不可变字节码与 flags，不解析外部输入、不分配可被外部规模放大的缓冲。无新增攻击面。

##### 敏感操作检查

**不涉及**敏感操作（无文件/网络/权限/进程控制）。

#### 8.4.7.4 可用性/性能分析

单次扫描在首次调用发生一次；命中缓存后该功能项不再执行（功能项 3）。扫描成本相对"被推迟的一次编译"可忽略，须以启动期 micro-bench 实测确认（R21，Outstanding）。

### 8.4.8 影响点列表

| 影响点 | 说明 |
|---|---|
| `cinderx/Jit/bytecode.*` | 复用归一接口；可能新增"isExceptionOpcode/家族查表"辅助 |
| `cinderx/Jit/osr.*` | 沿用后向边 opcode 语义；不调用 `collectBackedgeTargetOffsets`，端点在分类器扫描内收集 |
| 新增 `cinderx/Jit/behavior_classifier.*` | 承载本功能项主体 |

### 8.4.9 分配需求

承接需求文档 R1–R16、R19、R22–R25；为功能项 3（缓存）提供被缓存的 `StructureKey` 值；为功能项 4（接口）提供聚合身份输入。

---

## 8.5 功能项 2：特化观测旁路信号

> **⚠ v1 不实现（审校 T2.2）：** T3.1(b) 最小策略不读 `specialization_band`，本功能项整条 **defer 到 Phase-3**（与计数式真稳定性信号一起做）。下文为 Phase-3 设计参考；v1 的 `gate_view` 仅含 `structure_key + gate_context`（T3.4），不含 `specialization_band`。

### 8.5.1 功能概述

旁路统计 code object 中"已被解释器特化"的 opcode 比例，离散为带滞回的 `specialization_band`（low/mid/high），作为 gate 时**次级微调**的弱信号。**不进 `structure_key`、不作统计聚合维度**。覆盖 R11、KD6、R20（滞回部分）。

### 8.5.2 实现思路

借需求文档审查 Finding 2 的结论：特化路径 miss 时 `DEOPT_IF` + 退避并**保留特化形态**（已核实 `cinderx/Interpreter/3.14/Includes/ceval_macros.h` 的 `DEOPT_IF`/`backoff_counter`/`JUMP_TO_PREDICTED`），故"已特化"只证明"曾经热/曾单态"，不证明"当前单态"。因此本信号定位为**弱**：仅做小幅阈值微调，并以滞回避免边界抖动。采集与功能项 1 的归一遍历**同次完成但互不污染**——归一判维度喂 `structure_key`，原始特化态喂本观测（R22）。

### 8.5.3 实现设计

#### 8.5.3.1 特化态判定设计

在遍历中对每条指令读取其**特化态**（未归一的 specialized opcode 形态，区别于 `unspecialize` 的结果）。判定该 opcode 是否属于"可特化族的已特化变体"（如 `BINARY_OP_*_INT`、`LOAD_ATTR_SLOT` 等，参考既有 `BytecodeInstruction::specializedOpcode()` 的覆盖集合，`bytecode.cpp:153`）。

#### 8.5.3.2 比例与滞回设计

```
function readSpecializationBand(code):
    presence = specialized_count(code) / effective_specializable(code)   # R11
    prev = cachedBand(code)                       # 上次带，初始 = low
    # 滞回：进入高带与跌出高带用不同 cutoff（R20）
    band = applyHysteresis(presence, prev,
                           up={low->mid: U1, mid->high: U2},
                           down={high->mid: D2, mid->low: D1})   # U2>D2, U1>D1
    cacheBand(code, band)                          # 旁路缓存，原子读写
    return band
```

`presence` 可在每次 gate 重算，或缓存 + 每 N 次惰性刷新（Outstanding，影响 R21 开销与迁移频率）。

### 8.5.4 增量SR清单

| SR 编号 | 描述 | 对应需求 |
|---|---|---|
| SR-SPEC-01 | 提供 `readSpecializationBand(code)`，输出 low/mid/high | R11 |
| SR-SPEC-02 | 带滞回的带跃迁，进/出高带不同 cutoff | R20 |
| SR-SPEC-03 | 与归一遍历同次采集、互不污染；不进 structure_key | R22/R18 |

### 8.5.5 实现接口设计

#### 8.5.5.1 实现接口设计（说明）

弱信号接口，输出仅消费于功能项 4 的 gate 微调；严格禁止流入功能项 3 的聚合身份与下游统计聚合键。

#### 8.5.5.2 实现接口定义（逻辑接口，语言无关）

```
type SpecBand = enum { low, mid, high }
interface readSpecializationBand(code: ReadOnlyCode) -> SpecBand   # 自适应、带滞回
# 约束：SpecBand 不得作为 map/统计的 key 组成部分
```

### 8.5.6 功能规格设计

- 弱语义：仅产生有限幅度的阈值微调，单凭高 band 不得把高 deopt 风险函数判为"应提前编译"（AE10）。
- 旁路隔离：band 不出现在任何聚合键中（R18）。

### 8.5.7 DFX分析

#### 8.5.7.1 可靠性分析

##### FMEA分析

| 失效模式 | 原因 | 影响 | 缓解 |
|---|---|---|---|
| 误把"曾单态"当"现稳定" | 特化形态 miss 后保留 | 多态函数被高估，提前编译→deopt | 弱语义限幅 + AE10 多态回归；真稳定信号（计数式）列为 Deferred |
| 带抖动 | presence 在 cutoff 附近 | 相邻 gate 阈值翻转 | 滞回（R20）；惰性刷新降低频率 |

#### 8.5.7.2 可服务性分析

诊断 dump 可附带 `(presence, band)`，与 `structure_key` 分列展示，强调其旁路、非聚合属性。

#### 8.5.7.3 安全设计检查

##### 安全设计确认

只读 code object 字节码特化态，无副作用、无外部输入解析。

##### 敏感操作检查

**不涉及**。

#### 8.5.7.4 可用性/性能分析

若每次 gate 重扫，成本与功能项 1 同阶；可用惰性刷新摊薄。须实测刷新频率对准入路径开销影响（R21）。

### 8.5.8 影响点列表

| 影响点 | 说明 |
|---|---|
| `cinderx/Jit/bytecode.*` | 复用 `specializedOpcode()` 覆盖集判定特化态 |
| 新增 `behavior_classifier.*` | 承载旁路统计与滞回 |

### 8.5.9 分配需求

承接 R11、KD6、R20（滞回）；为功能项 4 提供 `specialization_band` 微调输入；与功能项 1 共享单次遍历。

---

## 8.6 功能项 3：structure_key 缓存与 free-threaded 发布

### 8.6.1 功能概述

把功能项 1 产出的 `structure_key` 缓存于 code object 的 `CodeExtra`，保证每 code object 仅计算一次，并在 FT 构建下遵守 release/acquire 发布契约与失败回退。分类 schema/config 在进入 gate/cache/policy 前冻结，缓存命中后不做运行期失效或版本比对（T3.11）。覆盖 R21、R26、KD8。

### 8.6.2 实现思路

已核实现状：`CodeExtra`（`cinderx/Common/code_extra.h:12`）当前只含 `calls`(union `next`)、`jit_compiled`、`jit_globals`、`jit_builtins`，**无分类缓存字段**；`jit_compiled` 以 `_Py_atomic_load_ptr_acquire` 读取、release 发布（`pyjit.cpp:3722` 附近）。`codeExtra(code)`（`cinderx/Common/code.cpp:185`）是 get-or-create，已在 `CriticalSectionGuard(code_obj)`（FT 锁，GIL 下 no-op）保护下分配 `PyMem_Calloc`（零初始化）。

方案：**扩展 `CodeExtra`** 增加单字发布字段 `skey_word`（bit31=`valid`，低位为打包的 `StructureKey` payload），沿用既有原子发布模型；并发首次计算因冻结配置下纯函数逐位相等而**良性**。单字 valid+payload 消除"值/标志"分离导致的排序风险；不存在独立的 `classified` 字段，也不存在运行期 invalidation/version 字段。

### 8.6.3 实现设计

#### 8.6.3.1 CodeExtra 扩展设计

新增字段：`uint32_t skey_word`（bit31 valid 位 + 低 16 bit `StructureKey` payload：mixed_shape + family + 结构修饰位，T3.10）。`PyMem_Calloc` 已零初始化 → `skey_word=0` 表"未分类"。字符串只用于 dump/log 诊断解码，不进入热路径、缓存或聚合存储。`specialization_band` 不属于 v1 `CodeExtra` 扩展，Phase-3 再单独设计。

#### 8.6.3.2 发布/读取设计

```
function getOrComputeStructureKey(code, extra):
    if extra == NULL: return INVALID         # 分配失败 → 回退（KD8(c)）
    word = acquire_load(extra.skey_word)     # 命中：valid + payload 同源
    if word.valid == 1:
        return unpack(word.payload)
    sk = deriveStructureKey(code)            # 功能项1，纯函数
    release_store(extra.skey_word, pack(sk) | VALID_BIT)
    return sk
    # 并发：多线程可同时走到此，sk 逐位相等，最后写入者胜出即可；
    #       或用 compare_exchange(skey_word,0,payload|VALID_BIT) 只发布一次（详细设计权衡）。
```

#### 8.6.3.3 失败回退设计

`codeExtra` 返回 NULL（shutdown、index 未初始化、`PyMem_Calloc`/`SetExtra` 失败，均见 `code.cpp:185`）时返回 `INVALID`，功能项 4 据此回退全局默认阈值，**绝不读取部分初始化状态**（KD8/R26）。

### 8.6.4 增量SR清单

| SR 编号 | 描述 | 对应需求 |
|---|---|---|
| SR-CACHE-01 | `CodeExtra` 扩展单字 `skey_word`（valid 位 + `StructureKey` payload） | R21/R26 |
| SR-CACHE-02 | release/acquire 发布与读取，遵守既有 FT 模型 | R26/KD8 |
| SR-CACHE-03 | 并发首次计算良性竞态（纯函数逐位相等）或 compare_exchange 单发布 | R26 |
| SR-CACHE-04 | 缓存不可用→返回 INVALID，由 gate 回退默认阈值 | KD8(c) |

### 8.6.5 实现接口设计

#### 8.6.5.1 实现接口设计（说明）

缓存接口对功能项 4 暴露 `getOrComputeStructureKey`（含发布逻辑）。所有跨线程可见字段经原子访问。Phase-3 若恢复 `band` 旁路读写，须与 `skey_word` 保持类型与聚合键隔离。

#### 8.6.5.2 实现接口定义（逻辑接口，语言无关）

```
interface getOrComputeStructureKey(code, extra) -> StructureKey | INVALID
# 扩展（集成边界，非新语言结构）：CodeExtra += { skey_word: packed-int with valid bit }
```

### 8.6.6 功能规格设计

- 每 code object `structure_key` 至多计算 O(线程数) 次（良性重复），命中后 O(1)。
- 读取永不撕裂、永不读半初始化状态。
- 分类配置进程内冻结；`skey_word` valid 后不失效、不版本比对、不重算。
- 回退路径不崩、不泄漏（失败时 `PyMem_Free` 已由既有 `codeExtra` 处理）。

### 8.6.7 DFX分析

#### 8.6.7.1 可靠性分析

##### FMEA分析

| 失效模式 | 原因 | 影响 | 缓解 |
|---|---|---|---|
| 读到半初始化 key | 值/标志分离发布 | 误用错误阈值 | `skey_word` 单字 release/acquire 发布，valid + payload 同源（8.6.3.2） |
| 撕裂读 | 非原子访问打包字段 | 错误 key | 打包为单机器字整型，原子读写 |
| 运行期修改分类配置 | cutoff/floor/δ/tie-break 等在已有 key 后变化 | 已缓存 key 与新配置不一致 | 不支持运行期修改；Phase 0 可反复调参，进入 gate/cache/policy 后配置冻结，变更需新进程或清空 code objects（T3.11） |
| 分配失败崩溃 | OOM/shutdown | 准入路径异常 | 返回 INVALID + 回退默认阈值（不崩） |

#### 8.6.7.2 可服务性分析

诊断可输出某 code object 的 `skey_word` valid 状态、解码后的 `structure_key`，辅助排查"为何走默认阈值"（即回退命中）。

#### 8.6.7.3 安全设计检查

##### 安全设计确认

复用 CPython code-extra 机制与既有临界区，无新增内存所有权风险（生命周期随 code object，`PyMem_Free` 回收）。

##### 敏感操作检查

涉及**并发内存发布**这一敏感点：必须 release/acquire 配对，禁止裸读写跨线程字段。已在 8.6.3.2/FMEA 约束。无文件/网络/权限操作。

#### 8.6.7.4 可用性/性能分析

命中后零额外成本；未命中一次扫描。FT 下良性重复计算最多 N(线程) 次，概率低且每次 O(n)，可接受。

### 8.6.8 影响点列表

| 影响点 | 说明 |
|---|---|
| `cinderx/Common/code_extra.h` | 扩展 `CodeExtra` 结构（新增字段 + 原子访问内联函数） |
| `cinderx/Common/code.cpp` | 复用 `codeExtra` get-or-create；可能微调以初始化新字段 |
| `cinderx/Jit/context.cpp` / `pyjit.cpp` | 参照其 `jit_compiled` release/acquire 范式实现新字段发布 |

### 8.6.9 分配需求

承接 R21、R26、KD8；向功能项 4 提供命中即 O(1) 的 `structure_key` 获取与回退信号。

---

## 8.7 功能项 4：分类器与准入点（jitVectorcall）逻辑接口集成

### 8.7.1 功能概述

在 AutoJIT 准入点 `jitVectorcall` 注入分类与 `computeThreshold(structure_key, gate_context, global)`（自由函数，T2.1）。v1 = 最小有用策略（明确 `raise_threshold_candidate` 抬阈值削减 compile storm，其余现状，T3.1b/T3.4/T3.5/T3.6）。**通过扩展 `PYTHONJITAUTO` 取值启用（不新增环境变量，T2.3）**：`=auto[:N]` 开分类，`=<N>`（数值）回到现状固定阈值。覆盖 R18、R26（回退）、KD8。特化观测 `specialization_band` 为 Phase-3 输入，v1 不读（T2.2）。

### 8.7.2 实现思路

最小侵入改造 `jitVectorcall`（`pyjit.cpp:183`，阈值门 `:197`）：在取 calls 后、决定解释/编译前，调用分类器取 `structure_key`，再读取不入 key 的 `gate_context`，交由自由函数 `computeThreshold(structure_key, gate_context, global)` 得到本次阈值。**v1 = 最小有用策略（T3.1b/T3.4/T3.5/T3.6）**：只对明确 `raise_threshold_candidate` 抬阈值，包括 `low_roi_candidate`（`Trivial` / `is_synthetic`）、`startup_init_candidate`（安全 import signal provider 已冻结来源的 `startup_phase` 且无 loop、非 static、主族偏 dynamic/dispatch/object/control）和 `compile_risk_defer_candidate`（`high_risk && loop_score==0 && !is_static`）；其余族走现状全局阈值。`high_risk` 不再等同低 ROI，避免误伤有循环或 static 的真实热函数。`startup_phase` 不是结构分类器的输出：不得在 `jitVectorcall` 内遍历 Python frame/code metadata 计算；Phase 0 先 dump 安全候选信号，证明覆盖主要可达 compile storm 且误伤可解释后，`readGateContext()` 才在热路径返回冻结后的 bool。**启用方式：复用 `PYTHONJITAUTO`（T2.3）**——把它从纯数值扩展为可接受 `auto[:N]`（FlagProcessor 已有 `void(const std::string&)` 重载，`jit_flag_processor.h:84`）：`=<N>`→固定阈值 N、分类关（现状不变，既有测试不受影响）；`=auto`→分类开、base 取默认；`=auto:N`→分类开、base=N。回退/对照 = 设回数值。特化观测 `specialization_band` 为 Phase-3 输入，v1 不读（T2.2）。`computeThreshold` 出现第二种策略时再提升为接口（T2.1）。

### 8.7.3 实现设计

#### 8.7.3.1 准入点改造设计（调用路径前后对比）

**改造前**

```
jitVectorcall(func):                          # pyjit.cpp:183，阈值门 :197（现状）
  limit = config.compile_after_n_calls
  if countCalls(code) < limit: return 解释路径
  return 编译路径
```

**改造后**

```
jitVectorcall(func):
  extra  = codeExtra(code)                                # gate helper 统一获取一次
  calls  = extra ? getCalls(extra) : 0
  global = config.compile_after_n_calls
  sk     = classifier.getOrComputeStructureKey(code, extra)  # 功能项3
  if sk == INVALID or not config.auto_classify:           # 回退 / PYTHONJITAUTO=数值（KD8/R26、T2.3）
      limit = global
  else:
      limit = computeThreshold(sk, gate_context, global)  # 功能项4：最小策略（T3.1b/T3.4/T3.5/T3.6/T2.1）
  if calls < limit: return 解释路径
  return 编译路径
# config.auto_classify 与 global 均由 PYTHONJITAUTO=auto[:N] / =N 解析得出（不新增环境变量）
```

#### 8.7.3.2 策略边界设计

`computeThreshold(structure_key, gate_context, global)`（自由函数，T2.1）是功能域对下游的**唯一阈值决策点**。v1 = 最小策略（`raise_threshold_candidate` 抬阈值，其余现状，T3.1b/T3.4/T3.5/T3.6）；下游可在不触碰分类器的前提下替换其实现或提升为接口（启发式 → 在线反馈）。统计聚合**必须**以 `structure_key` 为键（R18）；`gate_context` 不落库、不聚合。特化观测 `specialization_band` 为 Phase-3 输入，v1 不参与（T2.2）。

### 8.7.4 增量SR清单

| SR 编号 | 描述 | 对应需求 |
|---|---|---|
| SR-GATE-01 | `jitVectorcall` 注入分类与 `computeThreshold`，保持解释/编译二分 | R18 |
| SR-GATE-02 | `computeThreshold(structure_key, gate_context, global)` 最小策略：low_roi / startup-init / risk-defer candidate 抬阈值 | T3.1b/T3.4/T3.5/T3.6 |
| SR-GATE-03 | `PYTHONJITAUTO=<N>`（分类关）/ `structure_key` 无效 → 回退全局阈值，逐函数等价现状 | R26/KD8/T2.3 |
| SR-GATE-04 | `PYTHONJITAUTO` 解析扩展为 `auto[:N]`（FlagProcessor string 重载），数值路径不变 | T2.3 |

### 8.7.5 实现接口设计

#### 8.7.5.1 实现接口设计（说明）

对上游（`jitVectorcall`）暴露 gate helper 与 `classifyAndThreshold` 便捷封装（内部串联功能项 1+3 与 `computeThreshold`）。gate helper 负责统一取得一次 `CodeExtra`、读取 calls 与 `gate_context`；同一个 `extra` 传给 `structure_key` 缓存读写。v1 无 `band`，不暴露特化观测。

#### 8.7.5.2 实现接口定义（逻辑接口，语言无关）

```
interface computeThreshold(sk: StructureKey, ctx: GateContext, global: uint) -> uint   # 自由函数（T2.1）
interface readAutoJitGateState(code) -> { extra, calls, gate_context }                 # 一次 codeExtra get；startup_phase 来源来自安全 import signal provider
interface classifyAndThreshold(code, extra, gate_context) -> uint
    # 内部: if not config.auto_classify: return config.compile_after_n_calls   # PYTHONJITAUTO=N
    #       sk = getOrComputeStructureKey(code, extra)
    #       if sk==INVALID: return config.compile_after_n_calls
    #       return computeThreshold(sk, gate_context, config.compile_after_n_calls)
# PYTHONJITAUTO 解析：=N→{auto_classify=false, global=N}；=auto[:N]→{auto_classify=true, global=N|默认}
# 聚合契约: 任何 pattern 级统计的 key == structure_key（v1 无 band）
```

### 8.7.6 功能规格设计

- 行为等价性（分类关时）：`PYTHONJITAUTO=<N>`（数值）下编译时机与现状逐函数一致（回归基线）；`=auto` 时仅 `raise_threshold_candidate` 编译时机后移。
- 可回退：分类/缓存任一不可用 → 全路径退回现有全局阈值语义。
- 边界清晰：下游只见 `structure_key` + 弱 band，看不到内部维度/计数。

### 8.7.7 DFX分析

#### 8.7.7.1 可靠性分析

##### FMEA分析

| 失效模式 | 原因 | 影响 | 缓解 |
|---|---|---|---|
| 准入路径异常 | 分类器抛错/超时 | 函数永不编译或崩溃 | 分类器无异常路径（纯计算）；任何 INVALID→回退默认阈值 |
| 统计被切碎 | 误用 band 入聚合键 | 反馈学习失真 | 接口契约强制聚合键==structure_key；AE8/代码评审守门 |
| 灰度回退失败 | 改造耦合过深 | 无法快速止血 | 默认策略薄封装 + 单一注入点，开关即回退 |

#### 8.7.7.2 可服务性分析

提供开关：禁用分类（直接走默认阈值）以隔离问题；诊断可打印每函数 `(structure_key, mixed_shape, gate_context, diagnostic_bucket, chosen_limit, 是否回退)`。

#### 8.7.7.3 安全设计检查

##### 安全设计确认

仅在既有准入路径内增加只读分类与一次策略查询，无新增外部交互面。

##### 敏感操作检查

涉及**改变编译准入决策**（影响性能而非正确性）。约束：决策仅影响"何时编译"，不改变编译产物语义；最坏情形退回现状阈值。无文件/网络/权限敏感操作。

#### 8.7.7.4 可用性/性能分析

准入路径新增：一次缓存命中读取（O(1)）+ 一次策略查询（默认 O(1)）。`CodeExtra` 由 gate helper 统一取得一次，禁止 `countCalls(code)` 与 `getOrComputeStructureKey(code)` 分别触发 get-or-create。首次另含一次扫描。须以启动期与稳态 micro-bench 验证准入路径无显著回归（R21）。

### 8.7.8 影响点列表

| 影响点 | 说明 |
|---|---|
| `cinderx/Jit/pyjit.cpp` (`jitVectorcall`) | 注入分类与策略查询（唯一准入改造点） |
| 新增 `computeThreshold` 自由函数 | v1 最小策略（low_roi / startup-init / risk-defer candidate 抬阈值，T3.1b/T3.4/T3.5/T3.6/T2.1）；下游升级时提升为接口 |
| 配置/开关 | 扩展 `PYTHONJITAUTO` 解析为 `auto[:N]`（复用既有 env，不新增；FlagProcessor string 重载，T2.3） |

### 8.7.9 分配需求

承接 R18、R20、R26、KD2/KD8；对下游功能域（阈值映射、在线反馈）输出 `structure_key` + 弱 band 逻辑接口与聚合契约。

---

## 8.8 功能域级 DFX 与验证映射（汇总）

| 验证场景（需求 AE） | 覆盖功能项 | 验证要点 |
|---|---|---|
| AE1–AE7 | 功能项 1 | 各族 structure_key 正确派生 |
| AE8 | 功能项 1+3 | structure_key 确定性（不同预热/特化形态不影响身份） |
| AE9 | 功能项 1 | 正交性（每 opcode 唯一归属） |
| AE10 | Phase-3 功能项 2 | 多态下弱信号不误判（v1 不跑） |
| AE11 | 功能项 3 | FT 并发首次分类一致 + 分配失败回退 |
| AE12 | 功能项 1+3 | Mixed top-2 shape 编码/缓存/聚合身份保真 |

## 8.9 待决项（与需求 Outstanding 对齐）

- `startup_phase` 的安全 import signal provider：不得在 `jitVectorcall` 中遍历 Python frame/code metadata；候选方案包括 import machinery 侧轻量 depth/counter、thread-local import state 或等价安全来源。provider 复跑 Phase 0 dump 前，v1 不能宣称覆盖 ImportInit 收益目标。
- Phase-3：`specialization_band` 边界与滞回宽度、`specialization_presence` 重读频率（每 gate vs 惰性刷新）。

**审校（ce-doc-review 2026-06-01）决策已定**（记录于需求文档《审校决策》一节），对本设计的影响：
- **T3.1(b)/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10/T3.11**：§8.7 默认策略由 no-op 改为**最小策略**——对 low_roi / startup-init / risk-defer candidate 抬阈值削减 compile storm，其余族走现状阈值；不可达 module/class body 只进 Phase 0 `InitCodeDiagnostic`；`high_risk` 不再一刀切等同低 ROI；`Mixed` 记录 top-2 `mixed_shape`；未落 `Mixed` 的并列选族使用 benefit-first tie-break；`structure_key` 物理缓存固定为 32-bit `skey_word`，字符串仅作诊断展示；分类配置进程内冻结，缓存无运行期失效。2026-06-02 Phase 0 C++ gate-side dump 已通过 Mixed/family 红线，可冻结分类 schema/default 起点；`startup_phase` 来源未冻结，需安全 import signal provider。
- **T2.1**：`AutoJitPolicy` 虚类 → 自由函数 `computeThreshold(structure_key, gate_context, ...)`（§8.7）。
- **T2.2**：**功能项 2（特化观测）整条 defer 到 Phase-3**；v1 `gate_view` 含 `structure_key + gate_context`，§8.5 不在 v1 实现。
- **T2.3**：不新增环境变量，复用 `PYTHONJITAUTO=auto[:N]` 启用（功能项 4）。
- **T2.4/T3.4**：去 `ScalarCompute`，并移除不可达 `ImportInit` family，v1 family 8 个（§8.4）。
- **T3.2/T3.3/T3.9/T3.11**：Phase 0 C++ gate-side dump 已完成并通过分类红线；标定用混合语料 + 可 env 覆盖 cutoff；dump 必须区分 `InitCodeDiagnostic`、`mixed_shape`、low_roi / startup-init / risk-defer candidate 与 startup 候选信号 mask；进入热路径后配置不再变化。ImportInit 相关 startup 候选信号仍需安全 provider 复跑后冻结。

## 8.10 参考与可信源

第一可信源为项目代码，关键位置：
- `cinderx/Jit/pyjit.cpp:183`（`jitVectorcall` 准入点，阈值门 `:197`）、`:101`（`countCalls`/`codeExtra`）、`:96`（`required_code_flags`）、`:1160`/`:1199`（eligibility flags gate）、`:300`（`PYTHONJITAUTO` 注册）。
- `cinderx/Jit/jit_flag_processor.h:84`（`addOption` 的 `void(const std::string&)` 重载，支撑 `PYTHONJITAUTO=auto[:N]`，T2.3）。
- `cinderx/Jit/bytecode.cpp:106` 公有 `opcode()`（canonical + SP `EXTENDED_OPCODE_FLAG` 复合）、`:153`（`specializedOpcode` 覆盖集）；`uninstrumentedOpcode` 为 private 勿用（审校 T1.2）。
- `cinderx/Jit/osr.cpp:327` / `osr.h:159` `collectBackedgeTargetOffsets`（仅 target、上限 16）；loop_score 端点改为单次扫描内就地收集（审校 T1.3）。
- `cinderx/Common/code_extra.h:12`（`CodeExtra` 结构）；release/acquire 发布范式见 `cinderx/Jit/context.cpp:523`（`jit_compiled`），`code_extra.h` 的 `calls` 访问器为 relaxed/seq_cst（审校 T4.1）。
- `cinderx/Common/code.cpp:185`（`codeExtra` get-or-create + `CriticalSectionGuard`）。
- `cinderx/Jit/hir/preload.cpp:449`（`CI_CO_STATICALLY_COMPILED`）、`cinderx/Jit/hir/builder.cpp`（opcode 处理权威集合）。
- `cinderx/Interpreter/3.14/Includes/ceval_macros.h`（`DEOPT_IF`/`backoff_counter`/`JUMP_TO_PREDICTED`，特化弱语义依据）。
- 上游需求：`docs/brainstorms/2026-05-31-autojit-behavior-classification-requirements.md`（R1–R26、KD1–KD8、AE1–AE11）。
- Phase 0 C++ evidence：`scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md`、`summary-clean/summary.json`、`logs/autojit-phase0-gdb-debug-container-20260602-115858.log`、`logs/autojit-phase0-gdb-after-fix-20260602-120011.log`。
