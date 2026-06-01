---
date: 2026-05-31
topic: autojit-behavior-classification
---

# 自适应 AutoJIT：行为模式分类（Behavior Pattern Classification）

## Summary

为自适应 AutoJIT 定义一套**证据充分、生产完备**的函数行为分类方案：从字节码 + `co_flags` 提取一个**确定性的结构核**（衡量"做哪类工作 + 有多少热工作"），收敛为一个**有界、版本与特化鲁棒**的稳定 `structure_key`，作为下游策略 / 统计 / profile 的**唯一聚合身份**；另有一个**弱特化观测（specialization observation）**作为独立的 gate 时旁路信号（衡量"代码是否曾被解释器特化"），**不进入聚合键**。`structure_key` 把当前全局固定的 `compile_after_n_calls` 阈值，替换为"按行为模式区分"的编译准入依据。

本文档**只交付分类法本身**。阈值映射（`threshold = f(pattern)`）、pattern 级在线反馈、profile 持久化是明确的下游工作（见 Scope Boundaries）。

## Problem Frame

单一固定阈值（如 `PYTHONJITAUTO=2`）过于粗糙：低阈值在启动期制造 compile storm，编译开销泄漏进 benchmark 与冷启动 / 短生命周期 worker 的真实 workload；而当前"延迟编译护栏"本质是改变了 workload，而非教会编译器"何时值得编译"。

分类法是整条自适应路线的**地基**：阈值策略、ROI 反馈、风险修正全部以 **`structure_key`** 为聚合维度。成败标准不是"维度选得漂不漂亮"，而是这套 key 能否在生产里**稳定、廉价、无遗漏**地给每个 code object 打标，且**真实预测编译 ROI**。**关键不变量：聚合身份必须不随函数运行而漂移**——否则同一函数的 candidate/compile/reuse/deopt 统计会被切碎到多个 key 上，策略在一个 key 下学习却在另一个 key 下决策（这正是审查指出的高危缺陷，见 KD6/R18/R20）。

**判据：签名是编译 ROI（benefit − cost）的代理。** 一个维度只有当它能预测 benefit 或 cost 时才配占一个槽。这条判据贯穿全文——它既决定了维度的取舍，也暴露了"只看工作种类、不看热度与稳定性"会漏掉 JIT 收益的真正来源。

关键约束（已核实）：AutoJIT 准入发生在 `jitVectorcall`（`cinderx/Jit/pyjit.cpp:183`，阈值门在 `:197`）——`countCalls(code) < compile_after_n_calls` 时继续解释，否则编译。**此刻 HIR 尚不存在**（编译期才构建），故 gating 签名只能来自字节码 + `co_flags` + 解释器已沉淀的特化状态，不能消费 HIR。

## Key Decisions

- **KD1. gating 签名来源：bytecode + co_flags + 解释器特化状态。** 不消费 HIR（HIR 仅在编译后存在，只能做下游二级反馈）。

- **KD2. 交付边界：分类法 + 最小阈值策略（审校 T3.1 修订）。** 产出稳定 `structure_key`，并带一个**最小有用策略** `computeThreshold`：对确定低 ROI 的族（`ImportInit`/synthetic/`high_risk`/`Trivial`）抬高阈值削减启动期 compile storm，其余族走现状阈值。完整阈值映射、pattern 级在线反馈、特化观测、profile 持久化仍为下游（v1 不做）。v1 不实现特化观测（T2.2）。

- **KD3. Key 派生：Approach C —— 主族 + 结构修饰位 = 稳定 `structure_key`。** 6 个工作维度取主导轴决定 `family`；loop / suspend / static / risk 作为正交**结构修饰位**。`family + 结构修饰位` 即 `structure_key`，**完全确定、即聚合身份**（有界 key 空间，实测约 30–50 活跃 key）。特化观测（KD6）是独立旁路信号，**不并入 `structure_key`**。

- **KD4. risk 与工作维度类别不同。** 工作维度衡量"做哪类工作"（benefit 信号）；risk 衡量"编译多容易翻车"（cost / 安全信号）。risk 是 modifier，不是 family，对齐 issue "来源信息只做二级修正"。

- **KD5. 目标仅 Python 3.14+；只需覆盖两类 opcode。** 3.14 字节码只含：(1) **3.14 基础 opcode**（如 `BINARY_OP`/`CALL`/`SEND`/`JUMP_BACKWARD`/`TO_BOOL`，单字节 <256）；(2) **Static Python opcode**（`(n | EXTENDED_OPCODE_FLAG=0x200)`，取值 ≥512，已核实 `cinderx/Interpreter/3.14/cinder_opcode_ids.h`）。旧式 3.10/3.11 opcode（`BINARY_ADD`/`CALL_FUNCTION`/`JUMP_ABSOLUTE`…）在 3.14 已不存在，**不入表**（`builder.cpp` 仍保留它们只是因为该文件跨版本编译，与本特性无关）。分类按 opcode 家族查表，未来 minor 版本（3.15…）按家族扩表。

- **KD6. 特化观测是一个*弱*旁路信号，不是类型稳定性证明，也不进聚合键（审查 Finding 2 修订）。** CPython 自适应解释器会把热字节码特化为 `BINARY_OP_ADD_INT`、`LOAD_ATTR_SLOT` 等形态。但已核实（`cinderx/Interpreter/3.14/Includes/ceval_macros.h` 的 `DEOPT_IF` + `backoff_counter`/`advance_backoff_counter` + `JUMP_TO_PREDICTED`）：特化路径在 miss 时**带 guard 跳回基础语义并退避，而特化 opcode 形态原地保留**。因此"已特化"只证明*曾经热过 / 曾经单态*，**不代表当前单态稳定**——一个先单态预热、后被多态调用的函数仍会被记为高特化比例，恰是 ROI 可疑、deopt 风险升高之时。故本信号**降级**为 `specialization_presence`（弱"特化存在性"），仅作 gate 时**次级微调**输入，**绝不并入 `structure_key`**，也**不作统计聚合维度**。更强的真稳定性信号（hit/miss/deopt/backoff 计数）成本更高，列为 Deferred（见 Scope）。

- **KD7. 每个 opcode 只计入唯一一个工作维度；risk 从已分配计数派生，不重复计原始 opcode。** 这消除维度间双计数，使 `argmax` 选族稳定（见 R25 与 Known Accuracy Limits）。

- **KD8. `structure_key` 缓存须遵守 free-threaded 发布契约（审查 Finding 3 修订）。** CinderX 支持自由线程构建（`Py_GIL_DISABLED`，已核实 `cinderx/Common/code_extra.h:35`、`cinderx/Jit/config.h:157`）。**注（审校 T4.1）：`code_extra.h` 自身的 `calls` 访问器用的是 relaxed/seq_cst（`_Py_atomic_add_uint64`/`_load_uint64_relaxed`），并非 release/acquire；可沿用的 release/acquire 发布范式来自 `jit_compiled` 指针发布（`cinderx/Jit/context.cpp:523` `_Py_atomic_store_ptr_release`），`code_extra.h:26-27` 仅以注释描述该发布发生在别处。** `structure_key` 缓存须沿用 jit_compiled 范式，并定义初始化标志、并发首次调用的良性竞态语义、以及发布 / 分配失败时的回退（退回全局默认阈值），不得引入对部分初始化状态的读取（见 R26）。

## Requirements

### A. 工作维度（Work Dimensions，确定性，参与 argmax 选族）

> 6 个维度，每个附**已核实的 opcode 证据**（来自 `cinderx/Jit/hir/builder.cpp` + `cinderx/Jit/bytecode.cpp`）。每个 opcode 只归属一个维度（R25）。

R1. **`compute_score` — 算术 / 数值强度。** 3.14 基础：`BINARY_OP`（oparg 选运算）、`UNARY_*`、`COMPARE_OP`(+`_INT/FLOAT/STR` 特化)、`TO_BOOL*`；Static：`PRIMITIVE_BINARY_OP`、`PRIM_OP_*_INT/DBL`、`PRIMITIVE_COMPARE_OP`、`PRIMITIVE_UNARY_OP`、`CONVERT_PRIMITIVE`、`PRIMITIVE_BOX/UNBOX`。循环区域内的算术加权（见 R7）。

R2. **`control_score` — 分支 / CFG 复杂度 / merge 密度。** `POP_JUMP_IF_*`、`JUMP_IF_*_OR_POP`、`JUMP_FORWARD`、`FOR_ITER`；异常控制流 `SETUP_FINALLY/WITH/ASYNC_WITH`、`PUSH_EXC_INFO`、`CHECK_EXC_MATCH`、`CHECK_EG_MATCH`、`RERAISE`、`RAISE_VARARGS`、`WITH_EXCEPT_START`、`CLEANUP_THROW`。merge 密度≈唯一跳转目标数/指令数。**注：异常类 opcode 在此计入 control（CFG 复杂度），但 risk_score 不重复计原始 opcode，而是读取 control 已统计出的"异常子计数"（R8、R25）。**

R3. **`object_traffic_score` — 属性 / 容器 / 对象搬运。** `LOAD/STORE/DELETE_ATTR`(+特化)、`BINARY_SUBSCR/STORE_SUBSCR/DELETE_SUBSCR`(+特化)、`BUILD_LIST/MAP/SET/TUPLE/STRING/SLICE`、`LIST_APPEND/EXTEND`、`SET_ADD`、`DICT_MERGE/UPDATE`、`UNPACK_SEQUENCE/EX`、`GET_LEN/FAST_LEN`、`COPY/SWAP/ROT_*/DUP_TOP*`；Static：`SEQUENCE_GET/SET`、`STORE_FIELD`、`SEQ_LIST/TUPLE`、`BUILD_CHECKED_LIST/MAP`。

R4. **`dispatch_score` — Python 调用 / dispatch 密度。** `CALL`(+`CALL_FUNCTION/METHOD/KW/EX`)、`KW_NAMES`、`PUSH_NULL`；Static：`INVOKE_FUNCTION/METHOD/NATIVE`。

R5. **`suspend_score` — generator / coroutine / async。** `SEND`、`YIELD_VALUE/FROM`、`GET_AWAITABLE/AITER/ANEXT/YIELD_FROM_ITER`、`END_ASYNC_FOR`、`END_SEND`、`RETURN_GENERATOR`、`GEN_START`、`BEFORE_ASYNC_WITH`、`SETUP_ASYNC_WITH`。

R6. **`dynamic_score` — 反射 / 模板 / 动态代码。** 高密度 `LOAD/STORE_GLOBAL`、`LOAD/STORE_NAME`、`LOAD/STORE_DEREF`、`FORMAT_VALUE/SIMPLE/WITH_SPEC`、`BUILD_STRING/TEMPLATE/INTERPOLATION`、`CONVERT_VALUE`；synthetic 启发式：`co_filename` 形如 `<string>`/`<lambdifygenerated>`。**已知局限**：`globals()/locals()/eval` 呈现为 `LOAD_GLOBAL`+`CALL`，静态不可精确识别（记为假设）。

### B. 结构上下文信号（Structural Context，确定性，不参与 argmax，作修饰位）

R7. **`loop_score`（0–3，分级，取代原 has_loop）—— 最高价值的 benefit 信号。** JIT 收益由"循环内时间"主导，故循环结构必须分级而非 1 bit：`0`=无后向边；`1`=单层平坦循环；`2`=嵌套；`3`=深嵌套 / 多循环。**实现路径（审校 T1.3 修正）**：既有 `collectBackedgeTargetOffsets`（`cinderx/Jit/osr.cpp:327` / `osr.h:159`）只返回 backedge 的 target 偏移（去重、上限 16），**不提供 `{source,target}` 端点对**；而嵌套深度需要源+目标区间。故 `loop_score` 不复用该 API，而在分类器**自身单次字节码扫描内就地收集** `(source, getJumpTarget())`（扫描本就逐指令遍历），既避免第二遍扫描，又不引入不存在的依赖。`loop_score` 既参与 family 细分（compute + loop → NumericLoop），又是下游阈值的首要输入。

R8. **`risk_score`（派生 modifier，非 family）。** 由已分配计数派生（**不重复计原始 opcode**，KD7/R25）：高 `suspend_score`（async 状态机）+ 高 `dynamic_score` + control 的异常子计数 + 极大 code（`co_codelen`，编译成本）+ synthetic / 生成代码。映射为 `high_risk` 位。

R9. **`is_static`（modifier）。** 由 `CI_CO_STATICALLY_COMPILED`（已核实：`cinderx/Jit/hir/preload.cpp:449`、`inliner.cpp:172`）。Static Python 类型化函数编译收益高且可靠——独立成高置信修饰位，不再溶解进 compute/dispatch。

R10. **`is_suspendable`（modifier）。** 由 `co_flags` 的 `CO_GENERATOR/CO_COROUTINE/CO_ASYNC_GENERATOR`——无需扫码即可判定，比 opcode 扫描更可靠。

### C. 特化观测（Specialization Observation — 弱旁路信号，gate 时读取，不进 `structure_key`）

> **⚠ v1 不实现（审校 T2.2 决策）：** T3.1(b) 的最小策略只用 family + 结构修饰位、不读特化观测，故本节整条（R11、滞回、`spec_band`、AE10）**defer 到 Phase-3**，与计数式真稳定性信号一起做。下文保留设计意图供后续阶段参考；v1 的 `gate_view` 仅含 `structure_key`。

R11. **`specialization_presence` = 已特化 opcode 数 / 可特化 opcode 数（弱信号，审查 Finding 2 修订；分母经审校 T4.3 由"有效 opcode"改为"可特化 opcode"——只在能被解释器特化的 opcode 集合内取比例，避免被大量不可特化指令稀释）。** 解释器在 AutoJIT 观察前*已将热字节码特化*（`BINARY_OP_ADD_INT`、`LOAD_ATTR_SLOT` …，证实于 `cinderx/Jit/bytecode.cpp:293`）。**但这只是"特化存在性"，不是类型稳定性证明**：特化路径 miss 时 guard + 退避并保留特化形态（KD6，证实于 `ceval_macros.h` `DEOPT_IF`/`backoff_counter`），故单态预热后转多态仍记高比例。因此：(a) 仅作 gate 时**次级微调**（如对结构上已偏好编译的族略微提前 / 推迟），(b) **不并入 `structure_key`、不作聚合维度**（R18），(c) 离散为 low/mid/high 三带，并须配**滞回（hysteresis）**避免边界抖动（R20）。**采集分工**：扫描时用公有 `opcode()` 取归一后的 canonical opcode 以判定*属于哪个工作维度*（R22），同时用 `specializedOpcode() != opcode()` 旁路记录该 opcode *是否处于特化态*以累加本观测——两者互不污染。**用作阈值输入前必须有交替形态（单态→多态）验收测试**（AE10）。

### D. 预过滤（Pre-filters，在 argmax 之前判定，保证无遗漏）

R12. **ImportInit 预过滤（取代原 init_score 作 argmax 维度，Gap F）。** 缺 `CO_OPTIMIZED | CO_NEWLOCALS`（即 `cinderx/Jit/pyjit.cpp:96` 的 `required_code_flags`）或 `co_name == "<module>"` → 直接判 `ImportInit` 族，**绕过 argmax**。这把 argmax 从 7 个竞争者降到 6 个更干净的工作维度，并消除 dynamic∩init 重叠。

R13. **Trivial 预过滤。** 全部工作维度低于 floor（getter、forwarder、薄包装）→ 直接判 `Trivial`。

### E. 分桶（Bucketing）

R14. **基于密度而非原始计数。** 每个工作维度 `density = 维度计数 / 有效指令总数`，按固定 cutoff 离散为 `0/1/2/3`，并设**绝对计数下限 floor**，避免极小函数因 1 条指令触发假信号。本次给合理默认 cutoff；精确标定属下游经验调参。

### F. Pattern Key 派生（Approach C）

R15. **`family = argmax`(6 工作维度 bucket)** + 固定 tie-break 优先序（建议 `suspend > dynamic > compute > dispatch > control > object`，让安全 / 风险敏感族优先夺标）。映射：

| 主导维度 | family |
|---|---|
| compute | `NumericLoop`（由 `loop_score` modifier 区分：`loop≥1` 数值循环 / `loop0` 直线算术，原 `ScalarCompute` 并入，审校 T2.4） |
| control | `BranchFSM` |
| object | `ObjectManipulator` |
| dispatch | `CallDispatcher` |
| suspend | `AsyncStateMachine` |
| dynamic | `ReflectionMeta` |
| （预过滤）| `ImportInit` / `Trivial` |

R16. **`Mixed` 兜底：** 无维度主导（top-1 与 top-2 差距在 δ 内且均中等）→ `Mixed`，保守处理。与 `Trivial`/`ImportInit` 一起保证**零空洞**。

R17. **结构修饰位 = `{loop_score(0–3), is_suspendable, is_static, high_risk}`，全部确定。** 特化观测（`specialization_presence` 带）**不是修饰位**，独立携带。

R18. **两个明确分离的标识（审查 Finding 1 修订）：**
- **`structure_key = "{family}|{结构修饰位}"`**（如 `NumericLoop|loop3,static`、`AsyncStateMachine|susp,risk`、`ReflectionMeta|risk`）——**完全确定，是下游策略 / 统计 / profile 的唯一聚合身份**。candidate/compile/reuse/deopt 一律按 `structure_key` 聚合。
- **`gate_view = (structure_key, specialization_band)`**——仅用于**当次**编译准入的即时阈值微调，**不落库、不作聚合维度**。把特化带从聚合键中剥离，确保同一函数的统计永远归并到同一 `structure_key`，即使其特化带随预热迁移。

### G. 覆盖性、确定性与生产契约

R19. **穷尽：** 每个 code object 恰好映射一个 family；`ImportInit`/`Trivial`/`Mixed` 作 catch-all，零空洞。

R20. **聚合身份完全确定；特化观测是带滞回的旁路（审查 Finding 1 修订）：** **`structure_key`**（family + 结构修饰位）是静态字节码 + `co_flags` 的纯函数，对同一 code object 恒定，**就是聚合身份**。**`specialization_band`** 随函数预热演进、gate 时读取，仅进 `gate_view` 做即时微调，**永不参与聚合**；其 low/mid/high 跃迁须用**滞回阈值**（进入高带与跌出高带用不同 cutoff），避免边界反复抖动导致阈值在两次相邻 gate 间翻转。即：聚合统计稳定，准入微调有阻尼。

R21. **廉价：** `structure_key` 为对 `co_code` 的单次 O(n) 扫描，按 R26 发布进 `codeExtra`（与 `countCalls` 同处，`cinderx/Jit/pyjit.cpp:101`），每 code object 仅算一次。`specialization_presence` 可在 gate 时低成本重读（再扫特化位，或缓存上次值 + 惰性刷新）。准入判断本身不得成为新热点（issue 开放问题三）。

R22. **指令化 / 特化处理（双用，审校 T1.2 修正）：** 工作维度归类用**公有** `BytecodeInstruction::opcode()`（`cinderx/Jit/bytecode.cpp:106`，已 unspecialize 且对 SP 复合 `EXTENDED_OPCODE_FLAG`，喂 `structure_key`）——**不要**用 `private` 的 `uninstrumentedOpcode()`（返回未复合 flag 的 ≤255 原始字节，会漏掉全部 SP opcode）；**同时**用 `specializedOpcode() != opcode()` 旁路记录该 opcode 是否处于特化态（喂 `specialization_presence`，R11）。两路互不污染。`EXTENDED_ARG`/`EXTENDED_OPCODE` 作非语义跳过。

R23. **版本鲁棒：** 单一"opcode 家族 → 维度"映射表，按家族匹配（KD5）。新增版本只扩表。

R24. **Static Python 感知：** `PRIMITIVE_*`→compute、`INVOKE_*`→dispatch、`SEQUENCE_*/STORE_FIELD/SEQ_*`→object、`CAST/CONVERT_PRIMITIVE`→compute；并置 `is_static` 修饰位（R9）。

R25. **正交性：每个 opcode 只计入唯一一个工作维度。** risk（R8）从已分配计数*派生*，不重复计原始 opcode。这消除 control∩risk、dynamic∩dispatch、dynamic∩init 的双计数，使 argmax 选族稳定（见 Known Accuracy Limits）。

R26. **`codeExtra` 缓存的 free-threaded 发布契约（审查 Finding 3 修订）。** `structure_key` 缓存须沿用既有 `jit_compiled` 的 release/acquire 发布范式（`cinderx/Jit/context.cpp:523` `_Py_atomic_store_ptr_release`；`code_extra.h:26-27` 注释指明该范式）。具体：(a) 把不可变的 `structure_key`（建议紧凑整型打包，见 Outstanding）连同一个 **initialized 标志**以 release-store 发布，读侧 acquire-load 后才使用；(b) 并发首次调用允许各自计算——因 `structure_key` 是纯函数，结果逐位相等，竞态**良性**，最后写入者胜出即可（或用 `compare_exchange` 只发布一次）；(c) 若 `codeExtra` 分配或发布不可用，**回退到全局默认 `compile_after_n_calls`**，绝不读取部分初始化状态。`specialization_presence` 的惰性刷新同样以原子读写，不得撕裂。

## Known Accuracy Limits

> 诚实声明 bytecode-only gating 的精度上限，并给出缓解 / 去重规则。

- **L1. 静态构成 ≠ 执行构成（Gap D）。** density 平等计入冷热代码：一个静态上 90% 是异常处理字节码的函数，运行时可能 99.9% 走 happy path。这是 bytecode-only（KD1）的固有上限。**缓解：`loop_score`（R7）加权——循环是唯一可靠跟踪动态热度的静态信号，故循环结构在 family 细分与下游阈值中权重最高。** 残余偏差由下游 pattern 级 reuse/deopt 反馈纠正（超出本范围）。

- **L2. 维度非完全正交（Gap E）。** 概念上相关的维度（如高 control 常伴高 risk）会让 `argmax` 对小扰动敏感。**去重规则：** (a) 每 opcode 仅归一个工作维度（R25）；(b) risk 从已分配计数派生而非重复计原始 opcode（R8）；(c) module/class 体走 `ImportInit` 预过滤而非进 dynamic argmax（R12）；(d) 近似并列时落 `Mixed`（R16）而非强行夺标。

- **L3. 纯动态行为不可静态识别。** `globals()/eval` 类（R6 局限）靠密度 + synthetic 文件名启发式近似，非精确。

## Visualizations

### Gating 生命周期（分层签名落点）

```
call N 命中 jitVectorcall (pyjit.cpp:183，阈值门 :197)
  └─ countCalls(code)                          # 已有：每 code object 调用计数
  └─ structure_key = scanBytecode(code)  [按 R26 原子发布进 codeExtra，仅算一次]
        ├─ 6 工作维度计数（uninstrument+unspecialize 归一，每 opcode 唯一归属）
        ├─ density 分桶 0/1/2/3 (+floor)
        ├─ loop_score 0-3（复用 osr.h backedge 收集）
        ├─ 结构修饰位 {is_static, is_suspendable, high_risk(派生)}
        └─ family = argmax(6 工作维度) | tie-break | Mixed | 预过滤
        ⇒ structure_key = "family|结构修饰位"   # 确定，即聚合身份（落库）
  └─ specialization_band = 弱特化比例 + 滞回    # 旁路观测，gate 时读，不落库不聚合
  └─ gate_view = (structure_key, specialization_band)   # 仅当次微调
  └─ threshold = policy(structure_key, 微调=specialization_band)   # 下游：本次不做
  └─ if countCalls >= threshold: 编译
       # 下游统计 candidate/compile/reuse/deopt 一律按 structure_key 聚合
       # codeExtra 分配/发布失败 → 回退全局默认阈值（R26）
```

### 分层签名结构

```
structure_key 结构核 (确定，聚合身份)      特化观测 (弱旁路，不聚合)
┌─────────────────────────────┐          ┌──────────────────────────┐
│ 工作维度(argmax):            │          │ specialization_presence =│
│  compute control object      │          │   特化 opcode / 总数      │
│  dispatch suspend dynamic    │          │  -> low/mid/high + 滞回   │
│ 结构修饰位:                  │          └──────────┬───────────────┘
│  loop_score(0-3)             │                     │ 仅当次微调
│  is_static is_suspendable    │                     │ (不落库/不聚合)
│  high_risk(派生)             │                     │
└──────────┬──────────────────┘                     │
   argmax+tiebreak / 预过滤 / Mixed                   │
           ▼                                          │
   structure_key = "family|结构修饰位" ──┐            │
        (~30–50 活跃 key, 落库聚合)        ▼            ▼
                              gate_view = (structure_key, specialization_band)
                                         └─ 仅供当次编译准入阈值微调
```

## Acceptance Examples

- **AE1. 数值嵌套循环**（双重 for + `BINARY_OP` + 下标，已被解释器特化为 `BINARY_OP_*_INT`）
  - **Then:** `structure_key=NumericLoop|loop2`（聚合身份）；`specialization_band=high` 作旁路微调、**不并入 key**。覆盖 R1/R7/R11/R15/R18。

- **AE2. async 协程**（`co_flags` 含 `CO_COROUTINE`，含 `GET_AWAITABLE/SEND`）
  - **Then:** `structure_key=AsyncStateMachine|susp,risk`。覆盖 R5/R8/R10/R18。

- **AE3. Static Python 类型化数值函数**（`CI_CO_STATICALLY_COMPILED`，`PRIMITIVE_*`，单循环）
  - **Then:** `structure_key=NumericLoop|loop1,static`——`is_static` 不再被稀释。覆盖 R7/R9/R24。

- **AE4. 分发器**（`handler_map[k](*args)` 多调用，无循环）→ family=`CallDispatcher`。覆盖 R4。

- **AE5. 模块体 / 类体**（缺 `CO_OPTIMIZED|CO_NEWLOCALS`）→ 预过滤直判 family=`ImportInit`，**不进 argmax**。覆盖 R12/R19。

- **AE6. 薄 getter**（`return self._x`）→ 全维度低于 floor → 预过滤判 `Trivial`。覆盖 R13/R19。

- **AE7. synthetic 生成代码**（`co_filename=="<lambdifygenerated>"`，高 `LOAD_GLOBAL`/format）→ `structure_key=ReflectionMeta|risk`，以"更高阈值"替代当前对 synthetic code 的硬延迟护栏。覆盖 R6/R8。

- **AE8. `structure_key` 确定性回归：** 同一 code object 在不同预热程度下分类，**`structure_key`（family + 结构修饰位）必须完全一致**；`specialization_band` 允许随热化在相邻带迁移，但**绝不影响 `structure_key` 与聚合身份**。覆盖 R18/R20/R22。

- **AE9. 正交性回归：** 含 `LOAD_GLOBAL; CALL` 序列的函数，`LOAD_GLOBAL` 只增 dynamic、`CALL` 只增 dispatch，无双计数；异常 opcode 只增 control，risk 从其子计数派生。覆盖 R25/L2。

- **AE10. 特化观测多态回归（审查 Finding 2）：** 同一函数先以**单一参数/对象形态**预热（特化命中）、再以**交替形态**调用（触发 `DEOPT_IF` + 退避）。断言：(a) `structure_key` 全程不变；(b) `specialization_presence` 不被当作"类型稳定"——即在多态阶段，弱信号至多做次级微调，**不得**单凭它把高 deopt 风险函数判为"应提前编译"。证明降级语义落地。覆盖 R11/KD6。

- **AE11. free-threaded 并发首次分类（审查 Finding 3）：** 多线程在 `Py_GIL_DISABLED` 构建下并发首次调用同一未分类 code object。断言：(a) 各线程读到的 `structure_key` 逐位一致；(b) 无对部分初始化状态的读取（initialized 标志 acquire 后才用）；(c) 注入 `codeExtra` 分配失败时，回退全局默认阈值且不崩。覆盖 R26/KD8。

## Scope Boundaries

**本次交付（v1 = 分类法 + 最小策略，审校决策后）：** R1–R10、R12–R26（**R11 特化观测 defer**）+ Known Accuracy Limits + AE1–AE9、AE11（**AE10 defer**）+ 最小策略 `computeThreshold`（T3.1b）+ 复用 `PYTHONJITAUTO=auto[:N]` 启用（不新增环境变量，T2.3）。family 枚举 9 个（去 `ScalarCompute`，T2.4）。**前置门：编码前必跑分布 dump + 红线（T3.2）。**

**Deferred for later（明确后续）：**
- 完整阈值映射 `threshold = f(structure_key, ...)`（每族编译方向与具体值；v1 仅做最小策略）。
- **特化观测整条（审校 T2.2）**：`specialization_presence`/`spec_band`/滞回/AE10 → Phase-3，与下条计数式信号一起做。
- **真类型稳定性信号：** 基于 hit/miss/deopt/backoff 计数的稳定性度量（比 `specialization_presence` 强，KD6/审查 Finding 2），Phase-3 一等输入。
- pattern 级在线反馈：`candidate/compile/reuse/deopt/bailout` 统计（**一律按 `structure_key` 聚合**）与动态调阈值（issue Phase 3）。
- 跨 run profile 持久化 / 轻量 PGO（issue Phase 4）。
- `computeThreshold` 提升为多态策略接口（出现第二种策略时，T2.1）。
- bucket cutoff / floor / δ 的经验标定（v1 给可 env 覆盖的保守默认，正式标定按 T3.3 协议）。

**Outside this product's identity（明确不做）：**
- 消费 HIR / 运行期类型反馈进入 **gating 签名**（KD1；HIR 只做下游二级反馈）。
- ML 驱动的 pattern 选择。
- 跨 run native code / 机器码缓存复用（issue 第一阶段非目标）。

## Dependencies / Assumptions

- **依赖** `BytecodeInstruction` 公有接口：`opcode()`（canonical，含 SP flag 复合）`/oparg/isBranch/getJumpTarget/specializedOpcode`（`cinderx/Jit/bytecode.cpp`）——均已存在；`uninstrumentedOpcode/unspecialize` 为 `private`/底层辅助，**不直接调用**（审校 T1.2）。
- **依赖** OSR backedge 收集 / 计数设施（`cinderx/Jit/osr.h`）以导出 `loop_score`，并为下游反馈提供运行期热度。
- **依赖** `codeExtra` 承载缓存 `structure_key`，并沿用 `jit_compiled` 的 release/acquire 发布范式（`context.cpp:523`；FT 构建 `Py_GIL_DISABLED`）——见 R26/KD8。
- **依赖** `CI_CO_STATICALLY_COMPILED`（`preload.cpp:449`）判 `is_static`。
- **假设** `specialization_band` 配滞回后，其旁路微调不致策略在相邻 gate 间抖动（R20）——须实测确认。
- **假设** 弱特化观测仅作次级微调时不引入系统性误判（KD6）——AE10 须验证多态场景。
- **假设** 单次 `structure_key` 扫描 + 周期性特化位重读的总开销相对"被推迟的编译"可忽略（issue 开放问题三）——须实测。
- **假设** opcode 家族表覆盖 3.14 基础 opcode + Static Python opcode 两类（目标 3.14+），权威清单 = 3.14 opcode 集（`builder.cpp` 的 3.14 分支）+ `cinder_opcode_ids.h`（SP）。

## Outstanding Questions

**Resolve before planning：**
- 默认 bucket cutoff / floor / `loop_score` 嵌套阈值 / `specialization_band` 边界与**滞回宽度**：先给保守默认，还是先跑 pyperformance code object 分布采样再定？（建议先采样分布，验证 family 是否均衡、是否有族吞掉一切。）
- tie-break 优先序（R15）是否就用 `suspend>dynamic>compute>dispatch>control>object`？
- `specialization_presence` 的重读频率：每次 gate 重扫特化位，还是缓存 + 每 N 次惰性刷新？（影响 R21 开销与 R20 带迁移频率。）

**Deferred to planning：**
- `structure_key` 内存表示：字符串 vs 紧凑整型编码（family enum << k | 结构修饰位）——倾向整型，既作统计哈希键，又便于 R26 的原子打包发布。`specialization_band` 单独存放，不并入。
- `structure_key` 缓存失效语义（code object 通常不可变，预计无需失效；特化观测单独惰性刷新）。

## 审校决策（ce-doc-review 2026-06-01，已定）

> 来自一轮四 persona 审校。T1（源码事实错误）与 T4（一致性/锚点）已在文档内直接修复；**T2（范围裁剪）** 与 **T3（前提/分期）** 经逐项决策，结论如下，并已回灌正文。

**T3 — 前提/分期（决定整体形态）：**
- **T3.1 ✅ 采纳 (b)：v1 带最小有用策略。** 默认策略不再是 no-op，而是对**确定低 ROI** 的族（`ImportInit`、synthetic、`high_risk`、`Trivial`）抬高阈值，直接削减启动期 compile storm（对齐 issue Phase 2"把硬延迟护栏换成更高阈值"）；其余族走现状阈值。这把 v1 从"零收益脚手架"变为"最小可用切片"，端到端验证管线。**KD2 的"只分类"边界相应放宽为"分类 + 最小阈值策略"。**
- **T3.2 ✅ 编码前必跑分布 dump + 红线。** 实现前先用诊断 dump（功能设计 §8.4.7.2）跑 pyperformance + 一个真实应用 workload，出 family 直方图与 `Mixed` 占比。**红线：`Mixed` > 40% 或任一族 > 50% → 先调分类方案（保留 top-2 轴 / 对 Mixed 用完整 bucket 元组）再开工。** 该实验同时验证 T3.1(b) 最小策略瞄准的低 ROI 族占比。
- **T3.3 ✅ 定轻量标定协议。** (1) 标定语料用混合集（pyperformance + import/dispatch 密集的真实 workload，平衡目标按后者）；(2) cutoff/floor/δ/risk 比率做成**可按部署覆盖**（env/config），非编译期硬编码；(3) 文档记录 CPython 版本升级触发重标定。与 T3.2 实验合并跑。

**T2 — 范围裁剪：**
- **T2.1 ✅ `AutoJitPolicy` 虚类降为自由函数** `computeThreshold(structure_key, ...)`，内含 T3.1(b) 最小策略；出现第二种策略再提升为接口（YAGNI）。
- **T2.2 ✅ v1 整体砍掉特化观测。** 因 T3.1(b) 最小策略只用 family + 结构修饰位、不读 `specialization_band`，整条旁路（`spec_band` 字段、`readSpecializationBand`、滞回、AE10）v1 无消费者，**整体 defer 到 Phase-3**，与其后继的计数式真稳定性信号一起做。`gate_view` 在 v1 简化为只剩 `structure_key`。
- **T2.3 ✅ 不新增环境变量，复用 `PYTHONJITAUTO`（扩展取值）。** 把 `PYTHONJITAUTO` 从纯数值改为可接受 `auto[:N]`（已核实 FlagProcessor 有 `void(const std::string&)` 重载，`jit_flag_processor.h:84`）：
  - `PYTHONJITAUTO=<N>`（整数）→ 现状：固定阈值 N、**分类关**（不变，既有测试 `PYTHONJITAUTO=10` 不受影响）；
  - `PYTHONJITAUTO=auto` → **分类开**，base 阈值取默认；`PYTHONJITAUTO=auto:N` → 分类开、base=N。
  - A/B 对照 / 热路径止血 = 把值改回数字（分类关）。语义比独立布尔开关更融入现有使用场景，且与 INVALID 回退（仅分类失败时）正交。
- **T2.4 ✅ 砍掉 `ScalarCompute` 族。** compute 主导一律归 `NumericLoop`，由 `loop_score` modifier 区分有无循环（`NumericLoop|loop0` 即原 ScalarCompute）。family 枚举 10 → 9。

## Sources / Research

- `cinderx/Jit/pyjit.cpp:183` `jitVectorcall`（阈值门 `:197`）—— AutoJIT 准入点，签名注入位置。
- `cinderx/Jit/pyjit.cpp:101` `countCalls`/`codeExtra` —— 既有 per-code-object 计数与缓存，结构核复用此处。
- `cinderx/Jit/pyjit.cpp:96` `required_code_flags`(`CO_OPTIMIZED|CO_NEWLOCALS`) —— `ImportInit` 预过滤依据（R12）。
- `cinderx/Jit/hir/builder.cpp` —— opcode 处理集合，R1–R6 证据来源（取其 3.14 分支）。
- `cinderx/Interpreter/3.14/cinder_opcode_ids.h` —— Static Python opcode 定义（`EXTENDED_OPCODE_FLAG=0x200`，SP opcode ≥512），R24/家族表 SP 子表依据。
- `cinderx/Jit/bytecode.cpp:106` 公有 `opcode()`（canonical + SP flag）、`:153` `specializedOpcode()` —— 归一（R22）与特化位读取（R11）；`uninstrumentedOpcode` 为 private，勿用（审校 T1.2）。
- `cinderx/Jit/bytecode.cpp:293` —— 证实自适应特化形态存在于 code object（`specialization_presence` 的依据）。
- `cinderx/Interpreter/3.14/Includes/ceval_macros.h`（`DEOPT_IF`、`backoff_counter`/`advance_backoff_counter`、`JUMP_TO_PREDICTED`）—— 证实特化路径 miss 时 guard+退避、特化形态原地保留，是 KD6/R11 把特化观测降级为弱信号的依据（审查 Finding 2）。
- `cinderx/Common/code_extra.h:12` `CodeExtra`、`:35`/`cinderx/Jit/config.h:157` `Py_GIL_DISABLED` 支持；`code_extra.h` 的 `calls` 访问器为 relaxed/seq_cst（非 release/acquire）。release/acquire 发布范式见 `cinderx/Jit/context.cpp:523`（`jit_compiled`）——R26/KD8 沿用此范式（审查 Finding 3，锚点经审校 T4.1 修正）。
- `cinderx/Jit/osr.cpp:327` / `osr.h:159` `collectBackedgeTargetOffsets` —— 仅返回 backedge target 偏移（去重、上限 16），**不含 source 端点**；`loop_score`（R7）改为在分类器单次扫描内就地收集端点（审校 T1.3）。运行期 backedge 计数（`osr.h` 的 `BackedgeEntry`）可作下游热度反馈来源，非本期 gating 输入。
- `cinderx/Jit/hir/preload.cpp:449`、`inliner.cpp:172` —— `CI_CO_STATICALLY_COMPILED`，`is_static`（R9）依据。
- `cinderx/Jit/hir/hir_stats.cpp`（`PYTHONJITDUMPHIRSTATS`）—— HIR 层"按函数计指令/类型"先例；下游反馈可用，**不用于 gating 签名**（KD1）。
- GitHub issue: sisibeloved/cinderx#3《探索基于行为模式的自适应 AutoJIT 阈值策略》—— 需求母体。
