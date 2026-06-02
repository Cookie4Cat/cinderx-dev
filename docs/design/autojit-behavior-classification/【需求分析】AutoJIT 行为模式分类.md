---
date: 2026-05-31
topic: autojit-behavior-classification
---

# 需求分析文档 — 自适应 AutoJIT 行为模式分类

## Summary

为自适应 AutoJIT 定义一套**证据充分、可生产化推进**的函数行为分类方案：从字节码 + `co_flags` 提取一个**确定性的结构核**（衡量"做哪类工作 + 有多少热工作"），收敛为一个**有界、版本与特化鲁棒**的稳定 `structure_key`，作为下游策略 / 统计 / profile 的**唯一聚合身份**。`structure_key` 把当前全局固定的 `compile_after_n_calls` 阈值，替换为"按行为模式区分"的编译准入依据；弱特化观测（specialization observation）仅作为 Phase-3 旁路信号保留设计意图，v1 不实现、不进入聚合键。

本文档 v1 **交付分类法 + 最小阈值策略**：产出稳定 `structure_key`，并通过 `computeThreshold(structure_key, gate_context, global)` 只对明确 `raise_threshold_candidate` 抬阈值以削减 compile storm。v1 可以先编码非 startup 分支，但**v1 完成 / 宣称 ImportInit 收益 / 冻结生产默认策略**必须通过两类 release gate：安全 import signal provider 复跑通过，以及 `PYTHONJITAUTO=auto[:N]` 相对数值 `N` 的 A/B 策略验收。完整阈值映射（`threshold = f(pattern)`）、弱特化观测、pattern 级在线反馈、profile 持久化是明确的下游工作（见 Scope Boundaries）。

## Evidence Update（2026-06-02 Phase 0 C++ dump）

C++ Phase 0 dump 已在 `blue-98` / `cinderx-test` 上复跑并形成冻结证据，产物见 `scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md` 与 `summary-clean/summary.json`。clean 口径覆盖 pyperformance subset + django 专项，共 53 个 dump 进程、417389 条 record、10434 个 unique `record_key`；`gate_reachable=416381`（99.8%）、`storm_candidates_reached_threshold=30605`、`compiled_records=23446`、`gate_observations=104232`。`Mixed(storm)=873`（2.9%），未触发 `Mixed > 40%` 红线；最大 storm family 为 `CallDispatcher=8796`，未触发“任一族 > 50%”红线。由此可冻结的是 **schema/evidence 层**：Phase 0 dump schema、gate-side evidence 价值、family/Mixed 分类区分度、Mixed 红线和 family 区分能力。当前 bootstrap cutoff/floor/δ/loop defaults 只能作为**编码起点**，不能等同于生产 policy/default 冻结；生产默认值还需经过混合语料 A/B 与至少一组相邻 cutoff/floor/δ/loop 配置对比后冻结。

family 名称以 v1 文档为准：`BranchFSM`、`ObjectManipulator` 是规范名。已归档 Phase 0 C++ artifact 中若出现 `BranchyControl`、`ObjectAccess`，分别视为 `BranchFSM`、`ObjectManipulator` 的历史别名；后续 scanner/report/golden summary 必须输出规范名，或在比较前显式做 alias normalize，避免 schema 已冻而 artifact 名称漂移。

但 startup/import 信号不能一起冻结。gdb 修复前 crash 证据见 `logs/autojit-phase0-gdb-debug-container-20260602-115858.log`：SIGSEGV 栈为 `PyType_HasFeature -> autojit_phase0::unicodeAsStringNoError -> isImportFrame -> hasImportStack -> currentSignalMask -> recordGate -> jitVectorcall -> CPython import machinery`。这证明**不能在 `jitVectorcall` 热路径通过遍历 Python frame/code metadata 来实现 `import_stack`**。修复后 gdb 正常退出，见 `logs/autojit-phase0-gdb-after-fix-20260602-120011.log`。当前 C++ clean summary 的 `import_stack={}` 是 gdb 驱动的稳定性边界，不表示 ImportInit 不重要；Python-only Phase 0 曾显示 import stack 是 ImportInit storm 的主要信号。C++ clean summary 中 `module_initializing` 只覆盖 795/30605 个 storm，不能替代完整 ImportInit 判定；`early_window` 覆盖 22451 个 storm，但早期时间窗口只能作辅助/对照，不能单独成为默认策略来源。

## Problem Frame

单一固定阈值（如 `PYTHONJITAUTO=2`）过于粗糙：低阈值在启动期制造 compile storm，编译开销泄漏进 benchmark 与冷启动 / 短生命周期 worker 的真实 workload；而当前"延迟编译护栏"本质是改变了 workload，而非教会编译器"何时值得编译"。

分类法是整条自适应路线的**地基**：阈值策略、ROI 反馈、风险修正全部以 **`structure_key`** 为聚合维度。成败标准不是"维度选得漂不漂亮"，而是这套 key 能否在生产里**稳定、廉价、无遗漏**地给每个 code object 打标，且**真实预测编译 ROI**。**关键不变量：聚合身份必须不随函数运行而漂移**——否则同一函数的 candidate/compile/reuse/deopt 统计会被切碎到多个 key 上，策略在一个 key 下学习却在另一个 key 下决策（这正是审查指出的高危缺陷，见 KD6/R18/R20）。

**判据：签名是编译 ROI（benefit − cost）的代理。** 一个维度只有当它能预测 benefit 或 cost 时才配占一个槽。这条判据贯穿全文——它既决定了维度的取舍，也暴露了"只看工作种类、不看热度与稳定性"会漏掉 JIT 收益的真正来源。

关键约束（已核实）：AutoJIT 准入发生在 `jitVectorcall`（`cinderx/Jit/pyjit.cpp:183`，阈值门在 `:197`）——`countCalls(code) < compile_after_n_calls` 时继续解释，否则编译。**此刻 HIR 尚不存在**（编译期才构建），故 gating 签名只能来自字节码 + `co_flags` + 解释器已沉淀的特化状态，不能消费 HIR。

## Key Decisions

- **KD1. gating 签名来源：bytecode + co_flags + 解释器特化状态。** 不消费 HIR（HIR 仅在编译后存在，只能做下游二级反馈）。

- **KD2. 交付边界：分类法 + 最小阈值策略（审校 T3.1/T3.4/T3.5/T3.6 + Phase 0 C++ 证据修订）。** 产出稳定 `structure_key`，并带一个**最小有用策略** `computeThreshold(structure_key, gate_context, global)`：只对明确的 `raise_threshold_candidate` 抬阈值，包括低 ROI 形态（`Trivial` / 窄 synthetic 低 ROI：`is_synthetic && loop_score==0 && !is_static && family ∈ {ReflectionMeta, Trivial}`）、启动期可达的 `startup_init_candidate`、以及受限的编译风险延迟形态（`high_risk && loop_score==0 && !is_static`）；其余族走现状阈值。`high_risk` 本身只是成本/安全 modifier，不再等价于低 ROI；synthetic NumericLoop、static synthetic、或高 loop synthetic 在 v1 默认不直接抬阈值，需 A/B 证明后再纳入。Phase 0 C++ clean summary 已证明 gate-side schema 与 family/Mixed 分类可冻结，但 `gate_context.startup_phase` 的来源**尚未冻结**：`module_initializing` 覆盖不足，`early_window` 不能单独作为默认来源，`import_stack` 的原始 C++ frame 遍历实现已被 gdb 证明不安全。安全 import signal provider 是 **v1 release gate**：可以先分阶段实现非 startup 分支，但未通过 provider 复跑前，startup-init 分支必须关闭，且 v1 不能宣称覆盖 ImportInit 收益目标。完整阈值映射、pattern 级在线反馈、特化观测、profile 持久化仍为下游（v1 不做）。v1 不实现特化观测（T2.2）。

- **KD3. Key 派生：Approach C —— 主族 + Mixed 形态 + 结构修饰位 = 稳定 `structure_key`。** 6 个工作维度取主导轴决定 `family`；若落 `Mixed`，额外记录 canonical top-2 工作维度组合 `mixed_shape`（最多 15 种，非 `Mixed` 为 `none`）；loop / suspend / static / risk / synthetic 作为正交**结构修饰位**。`family + mixed_shape + 结构修饰位` 即 `structure_key`，**完全确定、即聚合身份**（有界 key 空间；Mixed 不再全部混成单桶）。特化观测（KD6）是独立旁路信号，**不并入 `structure_key`**。

- **KD4. risk 与工作维度类别不同。** 工作维度衡量"做哪类工作"（benefit 信号）；risk 衡量"编译多容易翻车"（cost / 安全信号）。risk 是 modifier，不是 family，也不自动代表低 ROI；v1 只有在 `loop_score==0 && !is_static` 时才把 `high_risk` 作为编译风险延迟候选，对齐 issue "来源信息只做二级修正"。

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

R4. **`dispatch_score` — Python 调用 / dispatch 密度。** `CALL`、`KW_NAMES`、`PUSH_NULL`；Static：`INVOKE_FUNCTION/METHOD/NATIVE`。旧式 `CALL_FUNCTION/METHOD/KW/EX` opcode 按 KD5 不进入 3.14+ 家族表。

R5. **`suspend_score` — generator / coroutine / async。** `SEND`、`YIELD_VALUE/FROM`、`GET_AWAITABLE/AITER/ANEXT/YIELD_FROM_ITER`、`END_ASYNC_FOR`、`END_SEND`、`RETURN_GENERATOR`、`GEN_START`、`BEFORE_ASYNC_WITH`、`SETUP_ASYNC_WITH`。

R6. **`dynamic_score` — 反射 / 模板 / 动态代码。** 高密度 `LOAD/STORE_GLOBAL`、`LOAD/STORE_NAME`、`LOAD/STORE_DEREF`、`FORMAT_VALUE/SIMPLE/WITH_SPEC`、`BUILD_STRING/TEMPLATE/INTERPOLATION`、`CONVERT_VALUE`；synthetic 启发式：`co_filename` 形如 `<string>`/`<lambdifygenerated>`。**已知局限**：`globals()/locals()/eval` 呈现为 `LOAD_GLOBAL`+`CALL`，静态不可精确识别（记为假设）。

### B. 结构上下文信号（Structural Context，确定性，不参与 argmax，作修饰位）

R7. **`loop_score`（0–3，分级，取代原 has_loop）—— 最高价值的 benefit 信号。** JIT 收益由"循环内时间"主导，故循环结构必须分级而非 1 bit：`0`=无后向边；`1`=单层平坦循环；`2`=嵌套或多个平坦循环；`3`=深嵌套 / 多循环。**bootstrap 映射（审校 T3.9，Phase 0 起跑用）：** `score = max(nesting_score, count_score)`；`nesting_score = min(max_static_nesting_depth, 3)`；`count_score = 0/1/2/3` 对应 backedge 数 `0 / 1 / 2–3 / >=4`。该映射进入 gate/cache/policy 前必须由 Phase 0 分布 dump 冻结或调整，不是永久调参结论。**实现路径（审校 T1.3 修正）**：既有 `collectBackedgeTargetOffsets`（`cinderx/Jit/osr.cpp:327` / `osr.h:159`）只返回 backedge 的 target 偏移（去重、上限 16），**不提供 `{source,target}` 端点对**；而嵌套深度需要源+目标区间。故 `loop_score` 不复用该 API，而在分类器**自身单次字节码扫描内就地收集** `(source, getJumpTarget())`（扫描本就逐指令遍历），既避免第二遍扫描，又不引入不存在的依赖。`loop_score` 既参与 family 细分（compute + loop → NumericLoop），又是下游阈值的首要输入。

R8. **`risk_score` 与 synthetic modifier（派生 modifier，非 family）。** `high_risk` 由已分配计数派生（**不重复计原始 opcode**，KD7/R25）：高 `suspend_score`（async 状态机）+ 高 `dynamic_score` + control 的异常子计数 + 极大 code（`co_codelen`，编译成本）。synthetic / generated code 由 `co_filename` 等稳定元数据映射为独立 `is_synthetic` 位，避免把"生成代码低 ROI"和"一般编译风险高"混在一个 `high_risk` bit 里。**（T3.6 对齐）`high_risk` 是编译成本/安全信号，不等于低 ROI**——其低 ROI 含义仅在下文 `loop_score==0 && !is_static` 的受限准入条件下成立，详见本条后半与 T3.6。v1 只把 `is_synthetic && loop_score==0 && !is_static && family ∈ {ReflectionMeta, Trivial}` 作为 synthetic 低 ROI candidate；synthetic NumericLoop、static synthetic、或高 loop synthetic 仍走全局阈值，除非 A/B 证明其编译收益低于编译成本。`high_risk` 只在 `loop_score==0 && !is_static` 时作为 `compile_risk_defer_candidate`，并需用 top call-count risk-defer candidate 的 saved compile cost 与 lost execution acceleration 对比来验证；若不能证明正 ROI，默认应禁用或按 family/code size 收窄。

R9. **`is_static`（modifier）。** 由 `CI_CO_STATICALLY_COMPILED`（已核实：`cinderx/Jit/hir/preload.cpp:449`、`inliner.cpp:172`）。Static Python 类型化函数编译收益高且可靠——独立成高置信修饰位，不再溶解进 compute/dispatch。

R10. **`is_suspendable`（modifier）。** 由 `co_flags` 的 `CO_GENERATOR/CO_COROUTINE/CO_ASYNC_GENERATOR`——无需扫码即可判定，比 opcode 扫描更可靠。

### C. 特化观测（Specialization Observation — 弱旁路信号，gate 时读取，不进 `structure_key`）

> **⚠ v1 不实现（审校 T2.2 决策）：** T3.1(b) 的最小策略不读特化观测，故本节整条（R11、滞回、`spec_band`、AE10）**defer 到 Phase-3**，与计数式真稳定性信号一起做。下文保留设计意图供后续阶段参考；v1 的 `gate_view` 仅含 `structure_key + gate_context`（T3.4），不含 `specialization_band`。

R11. **`specialization_presence` = 已特化 opcode 数 / 可特化 opcode 数（弱信号，审查 Finding 2 修订；分母经审校 T4.3 由"有效 opcode"改为"可特化 opcode"——只在能被解释器特化的 opcode 集合内取比例，避免被大量不可特化指令稀释）。** 解释器在 AutoJIT 观察前*已将热字节码特化*（`BINARY_OP_ADD_INT`、`LOAD_ATTR_SLOT` …，证实于 `cinderx/Jit/bytecode.cpp:293`）。**但这只是"特化存在性"，不是类型稳定性证明**：特化路径 miss 时 guard + 退避并保留特化形态（KD6，证实于 `ceval_macros.h` `DEOPT_IF`/`backoff_counter`），故单态预热后转多态仍记高比例。因此：(a) 仅作 gate 时**次级微调**（如对结构上已偏好编译的族略微提前 / 推迟），(b) **不并入 `structure_key`、不作聚合维度**（R18），(c) 离散为 low/mid/high 三带，并须配**滞回（hysteresis）**避免边界抖动（R20）。**采集分工**：扫描时用公有 `opcode()` 取归一后的 canonical opcode 以判定*属于哪个工作维度*（R22），同时用 `specializedOpcode() != opcode()` 旁路记录该 opcode *是否处于特化态*以累加本观测——两者互不污染。**用作阈值输入前必须有交替形态（单态→多态）验收测试**（AE10）。

### D. 预过滤（Pre-filters，在 argmax 之前判定，保证无遗漏）

R12. **初始化路径分流（替代原 `ImportInit` family，审校 T3.4/T3.5 + Phase 0 C++ 证据修订）。** 现有 AutoJIT 的 `jitVectorcall` gate 只覆盖通过 `required_code_flags = CO_OPTIMIZED | CO_NEWLOCALS` 的 `PyFunctionObject`；缺这些 flags 或 `co_name == "<module>"` 的 module/class body 在 v1 gate 上不可达。故 v1 不再把它们编码为 `structure_key` family，而是在 Phase 0 dump 中单独标为 **`InitCodeDiagnostic`**（诊断桶，不落 `skey_word`、不参与 `computeThreshold`）。startup/import compile storm 的 v1 收益目标仍是**可达的** `startup_init_candidate`：`gate_context.startup_phase == true`，且 `structure_key` 显示为低收益初始化形态（无 loop、非 static，主族偏 dynamic/dispatch/object/control，或已是 `Trivial`/窄 synthetic 低 ROI）。`startup_phase` 只作为 gate 当次上下文输入，不并入 `structure_key`，不切碎聚合统计。Phase 0 C++ 证据要求 `startup_phase` 来源按以下约束实现：不得在 `jitVectorcall` 内遍历 Python frame/code metadata 计算 `import_stack`（gdb 已定位 SIGSEGV）；不得只用 `module_initializing`（仅覆盖 795/30605 个 storm）；不得只用 `early_window`（时间窗口仅作辅助）。正式接入 startup-init 分支前必须实现安全 import signal provider（优先在 import machinery C 入口或等价安全点维护轻量 thread-local import depth/counter，例如围绕 `_PyEval_ImportName` / `_PyEval_LazyImportName` 及 from-import 对应入口；`readGateContext()` 只做 O(1) 读取），再用 dump 证明覆盖主要可达 ImportInit storm 且误伤可解释。provider 选择、gdb smoke 与 Phase 0.5 复跑是 v1 release gate；provider 未通过时 `startup_phase=false` 且 startup-init branch 不可用。

R13. **Trivial 预过滤。** 全部工作维度低于 floor（getter、forwarder、薄包装）→ 直接判 `Trivial`。

### E. 分桶（Bucketing）

R14. **基于密度而非原始计数；bootstrap defaults 只作 Phase 0 起跑（审校 T3.9/T3.11）。** 每个工作维度 `density = 维度计数 / 有效指令总数`，按 cutoff 离散为 `0/1/2/3`，并设**绝对计数下限 floor**，避免极小函数因 1 条指令触发假信号。bootstrap 初值：`count_floor=2`；density bucket cutoff 为 `0.10/0.25/0.50`（低/中/高）；`Mixed` 判定要求 top-1/top-2 bucket gap `<=1` 且二者 bucket `>=2`。这些值集中配置、可按部署覆盖，但覆盖只在 Phase 0 dump / 进程启动 / 进入 gate 前生效；Phase 0 C++ 只冻结 schema/evidence 与编码起点，不冻结生产 policy/default。进入 `jitVectorcall`/cache/policy 前，必须通过 pyperformance + import/dispatch 密集真实 workload 的 A/B，并至少比较一组相邻 cutoff/floor/δ/loop 设置，才能冻结生产默认值；冻结后进程内不再变化，否则不得声称 v1 默认策略已标定。

### F. Pattern Key 派生（Approach C）

R15. **`family = argmax`(6 工作维度 bucket) + benefit-first tie-break（审校 T3.8）。** 近并列先按 R16 落 `Mixed(+mixed_shape)`；未落 `Mixed` 的剩余并列场景用固定顺序 `compute > dispatch > object > control > dynamic > suspend`。`family` 只表达主要收益/工作类型，风险与成本由 `is_suspendable`、`high_risk`、`is_synthetic`、`gate_context` 等 modifier/context 处理，避免 tie-break 把风险策略混入主族。映射：

| 主导维度 | family |
|---|---|
| compute | `NumericLoop`（由 `loop_score` modifier 区分：`loop≥1` 数值循环 / `loop0` 直线算术，原 `ScalarCompute` 并入，审校 T2.4） |
| control | `BranchFSM` |
| object | `ObjectManipulator` |
| dispatch | `CallDispatcher` |
| suspend | `AsyncStateMachine` |
| dynamic | `ReflectionMeta` |
| （预过滤）| `Trivial` |

R16. **`Mixed` 兜底 + `mixed_shape`（审校 T3.7）：** 无维度主导（top-1 与 top-2 差距在 δ 内且均中等）→ `Mixed`，同时把 top-2 工作维度记录为 canonical unordered pair `mixed_shape`（如 `dynamic+dispatch`、`object+control`；最多 `C(6,2)=15` 种）。非 `Mixed` family 的 `mixed_shape=none`。这样保留 Mixed 的保守兜底，又避免所有近并列函数聚合成一个不可解释的大桶。与 `Trivial` 一起保证**零空洞**。不可达初始化代码由 `InitCodeDiagnostic` 诊断桶覆盖，不属于 v1 `structure_key` 空间。

R17. **结构修饰位 = `{loop_score(0–3), is_suspendable, is_static, high_risk, is_synthetic}`，全部确定；`mixed_shape` 是 Mixed-only 结构子形态。** 特化观测（`specialization_presence` 带）**不是修饰位**，独立携带。

R18. **两个明确分离的标识（审查 Finding 1 修订）：**
- **`structure_key = "{family}[mixed_shape]|{结构修饰位}"`**（如 `NumericLoop|loop3,static`、`AsyncStateMachine|susp,risk`、`ReflectionMeta|synthetic`、`Mixed(dynamic+dispatch)|loop0`）——**完全确定，是下游策略 / 统计 / profile 的唯一聚合身份**。candidate/compile/reuse/deopt 一律按 `structure_key` 聚合。
- **`gate_view = (structure_key, gate_context)`**——仅用于**当次**编译准入的即时阈值选择，**不落库、不作聚合维度**。v1 的 `gate_context` 仅含 startup/import 上下文（如 `startup_phase`）；Phase-3 才追加 `specialization_band` 旁路。把上下文/特化带从聚合键中剥离，确保同一函数的统计永远归并到同一 `structure_key`。

### G. 覆盖性、确定性与生产契约

R19. **穷尽：** 每个 AutoJIT gate 可达的 code object 恰好映射一个 family；`Trivial`/`Mixed` 作 catch-all，零空洞。不可达的 module/class body 由 Phase 0 `InitCodeDiagnostic` 覆盖，并在分布报告中单列。

R20. **聚合身份完全确定；gate 上下文 / 特化观测均不入聚合（审查 Finding 1 + T3.4/T3.7 修订）：** **`structure_key`**（family + Mixed-only `mixed_shape` + 结构修饰位）是静态字节码 + `co_flags` 的纯函数，对同一 code object 恒定，**就是聚合身份**。`gate_context.startup_phase` 随调用时机变化，v1 仅进 `gate_view` 做当次阈值选择，**永不参与聚合**。**`specialization_band`** 随函数预热演进、gate 时读取，Phase-3 才加入 `gate_view` 做即时微调，**永不参与聚合**；其 low/mid/high 跃迁须用**滞回阈值**（进入高带与跌出高带用不同 cutoff），避免边界反复抖动导致阈值在两次相邻 gate 间翻转。即：聚合统计稳定，准入微调有阻尼。

R21. **廉价：** `structure_key` 为对 `co_code` 的单次 O(n) 扫描，按 R26 发布进 `codeExtra`（与 `countCalls` 同处，`cinderx/Jit/pyjit.cpp:101`），每 code object 仅算一次。`specialization_presence` 可在 gate 时低成本重读（再扫特化位，或缓存上次值 + 惰性刷新）。准入判断本身不得成为新热点（issue 开放问题三）。

R22. **指令化 / 特化处理（双用，审校 T1.2 修正）：** 工作维度归类用**公有** `BytecodeInstruction::opcode()`（`cinderx/Jit/bytecode.cpp:106`，已 unspecialize 且对 SP 复合 `EXTENDED_OPCODE_FLAG`，喂 `structure_key`）——**不要**用 `private` 的 `uninstrumentedOpcode()`（返回未复合 flag 的 ≤255 原始字节，会漏掉全部 SP opcode）。Phase-3 才用 `specializedOpcode() != opcode()` 旁路记录该 opcode 是否处于特化态（喂 `specialization_presence`，R11）；v1 不采集、不缓存、不读取特化观测。两路互不污染。`EXTENDED_ARG`/`EXTENDED_OPCODE` 作非语义跳过。

R23. **版本鲁棒：** 单一"opcode 家族 → 维度"映射表，按家族匹配（KD5）。新增版本只扩表。

R24. **Static Python 感知：** `PRIMITIVE_*`→compute、`INVOKE_*`→dispatch、`SEQUENCE_*/STORE_FIELD/SEQ_*`→object、`CAST/CONVERT_PRIMITIVE`→compute；并置 `is_static` 修饰位（R9）。

R25. **正交性：每个 opcode 只计入唯一一个工作维度。** risk（R8）从已分配计数*派生*，不重复计原始 opcode；synthetic/generated 从稳定元数据派生为 `is_synthetic`，不参与 opcode 计数。这消除 control∩risk、dynamic∩dispatch、dynamic∩init 的双计数，使 argmax 选族稳定（见 Known Accuracy Limits）。

R26. **`codeExtra` 缓存的 free-threaded 发布契约（审查 Finding 3 + T3.10/T3.11 修订）。** `structure_key` 缓存须沿用既有 `jit_compiled` 的 release/acquire 发布范式（`cinderx/Jit/context.cpp:523` `_Py_atomic_store_ptr_release`；`code_extra.h:26-27` 注释指明该范式）。v1 物理表示固定为 **`uint32_t skey_word`**：bit31 是 valid 位，低 16 bit 是 `StructureKey` payload（`mixed_shape + family + modifiers`），字符串只用于诊断 dump/log 的解码展示，不进入热路径、缓存或聚合存储。分类 schema/config（opcode 家族表、cutoff/floor/δ/loop 阈值、tie-break、payload 位布局）在进入 gate/cache/policy 前冻结为进程内常量；`skey_word` valid 后**不失效、不做运行期版本比对、不重算**。调参只能在 Phase 0 scanner/dump 或新进程中进行；变更已冻结配置需要重启进程或清空 code objects。具体：(a) 把不可变的 `structure_key` payload 与 valid 位合并为单字，以 release-store 发布，读侧一次 acquire-load 后才 unpack 使用；(b) 并发首次调用允许各自计算——因 `structure_key` 是冻结配置下的纯函数，结果逐位相等，竞态**良性**，最后写入者胜出即可（或用 `compare_exchange` 只发布一次）；(c) 若 `codeExtra` 分配或发布不可用，**回退到全局默认 `compile_after_n_calls`**，绝不读取部分初始化状态。`specialization_presence` 的惰性刷新同样以原子读写，不得撕裂。

## Known Accuracy Limits

> 诚实声明 bytecode-only gating 的精度上限，并给出缓解 / 去重规则。

- **L1. 静态构成 ≠ 执行构成（Gap D）。** density 平等计入冷热代码：一个静态上 90% 是异常处理字节码的函数，运行时可能 99.9% 走 happy path。这是 bytecode-only（KD1）的固有上限。**缓解：`loop_score`（R7）加权——循环是唯一可靠跟踪动态热度的静态信号，故循环结构在 family 细分与下游阈值中权重最高。** 残余偏差由下游 pattern 级 reuse/deopt 反馈纠正（超出本范围）。

- **L2. 维度非完全正交（Gap E）。** 概念上相关的维度（如高 control 常伴高 risk）会让 `argmax` 对小扰动敏感。**去重规则：** (a) 每 opcode 仅归一个工作维度（R25）；(b) risk 从已分配计数派生而非重复计原始 opcode（R8）；(c) module/class 体在 Phase 0 走 `InitCodeDiagnostic` 诊断桶而非进 dynamic argmax（R12）；(d) 近似并列时落 `Mixed` 并记录 canonical top-2 `mixed_shape`（R16），而非强行夺标或把所有 Mixed 混成单桶。

- **L4. startup/import 上下文不是结构身份。** `startup_phase` 影响"此刻是否值得编译"，但不是 code object 的静态结构属性，故不得并入 `structure_key`。Phase 0 必须同时报告 `InitCodeDiagnostic`（不可达）与 `startup_init_candidate`（可达）占比，并输出 startup 候选信号 mask 的分布与命中率。2026-06-02 C++ clean summary 显示 gate-side storm 主要可达（`gate_reachable=99.8%`），但 `module_initializing` 覆盖不足且原始 `import_stack` C++ 实现不安全；因此 v1 不能声称已解决 ImportInit 收益目标，除非先实现安全 import signal provider 并复跑 dump 证明覆盖率/误伤率。

- **L3. 纯动态行为不可静态识别。** `globals()/eval` 类（R6 局限）靠密度 + synthetic 文件名启发式近似，非精确。

## Visualizations

### Gating 生命周期（分层签名落点）

```
call N 命中 jitVectorcall (pyjit.cpp:183，阈值门 :197)
  └─ gate_state = readAutoJitGateState(code)   # 一次 codeExtra get，读取 calls
  └─ structure_key = scanBytecode(code)  [按 R26 原子发布进 codeExtra，仅算一次]
        ├─ 6 工作维度计数（uninstrument+unspecialize 归一，每 opcode 唯一归属）
        ├─ density 分桶 0/1/2/3 (+floor)
        ├─ loop_score 0-3（沿用 OSR 后向边语义；分类器单次扫描内收集 source/target）
        ├─ 结构修饰位 {is_static, is_suspendable, high_risk(派生), is_synthetic}
        └─ family = argmax(6 工作维度) | tie-break | Mixed(+mixed_shape) | 预过滤
        ⇒ structure_key = "family[mixed_shape]|结构修饰位"   # 确定，即聚合身份（落库）
  └─ gate_context = {startup_phase, ...}  # 不入 structure_key，不聚合
  └─ threshold = computeThreshold(structure_key, gate_context, global)  # v1 最小策略
       # Phase-3 才加入 specialization_band 旁路微调；不落库、不聚合
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
│  high_risk / is_synthetic    │                     │
└──────────┬──────────────────┘                     │
   argmax+tiebreak / 预过滤 / Mixed(+top2 shape)       │
           ▼                                          │
   structure_key = "family[mixed_shape]|结构修饰位" ──┐ │
        (~30–50 活跃 key, 落库聚合)        ▼            ▼
                              gate_view = (structure_key, gate_context[, specialization_band])
                                         └─ 仅供当次编译准入阈值选择
```

## Acceptance Examples

- **AE1. 数值嵌套循环**（双重 for + `BINARY_OP` + 下标，已被解释器特化为 `BINARY_OP_*_INT`）
  - **Then:** `structure_key=NumericLoop|loop2`（聚合身份）。覆盖 R1/R7/R15/R18。Phase-3 的 `specialization_band` 旁路验证见 AE10。

- **AE2. async 协程**（`co_flags` 含 `CO_COROUTINE`，含 `GET_AWAITABLE/SEND`）
  - **Then:** `structure_key=AsyncStateMachine|susp,risk`。覆盖 R5/R8/R10/R18。

- **AE3. Static Python 类型化数值函数**（`CI_CO_STATICALLY_COMPILED`，`PRIMITIVE_*`，单循环）
  - **Then:** `structure_key=NumericLoop|loop1,static`——`is_static` 不再被稀释。覆盖 R7/R9/R24。

- **AE4. 分发器**（`handler_map[k](*args)` 多调用，无循环）→ family=`CallDispatcher`。覆盖 R4。

- **AE5. 初始化路径可达性回归。**
  - module/class body（缺 `CO_OPTIMIZED|CO_NEWLOCALS` 或 `co_name=="<module>"`）→ Phase 0 dump 标为 `InitCodeDiagnostic`，不生成 v1 `structure_key`，不进入 `computeThreshold`。覆盖 R12/R19。
  - startup/import 阶段被 `jitVectorcall` 命中的普通函数（通过 required flags、无 loop、非 static、dynamic/dispatch/object/control 主导）→ 生成稳定 `structure_key`；安全 import signal provider 经 Phase 0 dump 证明并冻结 `gate_context.startup_phase=true` 来源后，由 `computeThreshold` 抬阈值。覆盖 KD2/R12/R18。

- **AE6. 薄 getter**（`return self._x`）→ 全维度低于 floor → 预过滤判 `Trivial`。覆盖 R13/R19。

- **AE7. synthetic 生成代码**（`co_filename=="<lambdifygenerated>"`，高 `LOAD_GLOBAL`/format，无 loop、非 static）→ `structure_key=ReflectionMeta|synthetic`（可同时带 `risk`），作为 v1 synthetic 低 ROI candidate 抬阈值；synthetic NumericLoop、static synthetic、或高 loop synthetic 不因 `is_synthetic` 单独抬阈值，需 A/B 证明后再纳入。普通 `high_risk` 不因此被一刀切视为低 ROI。覆盖 R6/R8/T3.6。

- **AE8. `structure_key` 确定性回归：** 同一 code object 在不同预热程度下分类，**`structure_key`（family + Mixed-only `mixed_shape` + 结构修饰位）必须完全一致**，不随解释器特化形态变化而漂移。覆盖 R18/R20/R22。`specialization_band` 迁移验证 defer 到 Phase-3/AE10。

- **AE9. 正交性回归：** 含 `LOAD_GLOBAL; CALL` 序列的函数，`LOAD_GLOBAL` 只增 dynamic、`CALL` 只增 dispatch，无双计数；异常 opcode 只增 control，risk 从其子计数派生。覆盖 R25/L2。

- **AE12. Mixed 形态保真（审校 T3.7）：** 一个 dynamic 与 dispatch 维度近并列的函数、一个 object 与 control 维度近并列的函数都应落 `Mixed`，但 `structure_key` 分别为 `Mixed(dynamic+dispatch)|...` 与 `Mixed(object+control)|...`；非 `Mixed` 函数的 `mixed_shape=none`。覆盖 R16/R18。

- **AE10. 特化观测多态回归（审查 Finding 2）：** 同一函数先以**单一参数/对象形态**预热（特化命中）、再以**交替形态**调用（触发 `DEOPT_IF` + 退避）。断言：(a) `structure_key` 全程不变；(b) `specialization_presence` 不被当作"类型稳定"——即在多态阶段，弱信号至多做次级微调，**不得**单凭它把高 deopt 风险函数判为"应提前编译"。证明降级语义落地。覆盖 R11/KD6。

- **AE11. free-threaded 并发首次分类（审查 Finding 3）：** 多线程在 `Py_GIL_DISABLED` 构建下并发首次调用同一未分类 code object。断言：(a) 各线程读到的 `structure_key` 逐位一致；(b) 无对部分初始化状态的读取（initialized 标志 acquire 后才用）；(c) 注入 `codeExtra` 分配失败时，回退全局默认阈值且不崩。覆盖 R26/KD8。

## Scope Boundaries

**本次交付（v1 = 分类法 + 最小策略，审校决策 + Phase 0 C++ 证据后）：** R1–R10、R12–R26（**R11 特化观测 defer**）+ Known Accuracy Limits + AE1–AE9、AE11–AE12（**AE10 defer**）+ 最小策略 `computeThreshold(structure_key, gate_context, global)`（T3.1b/T3.4/T3.5/T3.6/T3.7/T3.8/T3.9/T3.10）+ 复用 `PYTHONJITAUTO=auto[:N]` 启用（不新增环境变量，T2.3）。family 枚举 8 个（去 `ScalarCompute` 与不可达 `ImportInit`；不可达初始化代码进 `InitCodeDiagnostic` 诊断桶），`Mixed` 子形态最多 15 个；未落 `Mixed` 的并列选族用 benefit-first tie-break；bootstrap cutoff/floor/loop defaults 已通过 C++ gate-side 红线，可作为 v1 编码起点；`structure_key` 物理缓存固定为 32-bit `skey_word`，字符串仅用于诊断。**v1 release gates：** (1) 安全 import signal provider 通过 gdb smoke + Phase 0.5 dump 后，startup-init 分支才可启用，v1 才能宣称覆盖 ImportInit 收益；(2) `PYTHONJITAUTO=auto[:N]` 相对数值 `N` 的 A/B 必须证明 compile storm 场景减少 `raise_threshold_candidate` 编译次数和编译总耗时，且启动/吞吐无显著回归、非 candidate 编译行为不变；(3) 至少比较一组相邻 cutoff/floor/δ/loop 配置后，才能冻结生产 policy/default；(4) synthetic 高 loop/static/generated 与 risk-defer candidate 需分别证明 saved compile cost 大于 lost execution acceleration，否则默认禁用或收窄。

**Deferred for later（明确后续）：**
- 完整阈值映射 `threshold = f(structure_key, ...)`（每族编译方向与具体值；v1 仅做最小策略）。
- **特化观测整条（审校 T2.2）**：`specialization_presence`/`spec_band`/滞回/AE10 → Phase-3，与下条计数式信号一起做。
- **真类型稳定性信号：** 基于 hit/miss/deopt/backoff 计数的稳定性度量（比 `specialization_presence` 强，KD6/审查 Finding 2），Phase-3 一等输入。
- pattern 级在线反馈：`candidate/compile/reuse/deopt/bailout` 统计（**一律按 `structure_key` 聚合**）与动态调阈值（issue Phase 3）。
- 跨 run profile 持久化 / 轻量 PGO（issue Phase 4）。
- `computeThreshold` 提升为多态策略接口（出现第二种策略时，T2.1）。
- post-v1 经验重标定：bucket cutoff / floor / δ / loop count 阈值可按 T3.3 协议随 CPython 版本或 workload 变化重新冻结；冻结后的新配置需新进程或清空 code objects 才能进入 gate/cache/policy。

**Outside this product's identity（明确不做）：**
- 消费 HIR / 运行期类型反馈进入 **gating 签名**（KD1；HIR 只做下游二级反馈）。
- ML 驱动的 pattern 选择。
- 跨 run native code / 机器码缓存复用（issue 第一阶段非目标）。

## Dependencies / Assumptions

- **依赖** `BytecodeInstruction` 公有接口：`opcode()`（canonical，含 SP flag 复合）`/oparg/isBranch/getJumpTarget/specializedOpcode`（`cinderx/Jit/bytecode.cpp`）——均已存在；`uninstrumentedOpcode/unspecialize` 为 `private`/底层辅助，**不直接调用**（审校 T1.2）。
- **依赖** OSR 后向边 opcode 语义 / 计数设施（`cinderx/Jit/osr.h`）作为 `loop_score` 语义参照；`loop_score` 的 source/target 端点由分类器单次字节码扫描内就地收集。
- **依赖** `codeExtra` 承载缓存 `structure_key`，并沿用 `jit_compiled` 的 release/acquire 发布范式（`context.cpp:523`；FT 构建 `Py_GIL_DISABLED`）——见 R26/KD8。
- **依赖** `CI_CO_STATICALLY_COMPILED`（`preload.cpp:449`）判 `is_static`。
- **Phase-3 假设** `specialization_band` 配滞回后，其旁路微调不致策略在相邻 gate 间抖动（R20）——须实测确认。
- **Phase-3 假设** 弱特化观测仅作次级微调时不引入系统性误判（KD6）——AE10 须验证多态场景。
- **假设** 单次 `structure_key` 扫描的总开销相对"被推迟的编译"可忽略（issue 开放问题三）——须实测。
- **假设** opcode 家族表覆盖 3.14 基础 opcode + Static Python opcode 两类（目标 3.14+），权威清单 = 3.14 opcode 集（`builder.cpp` 的 3.14 分支）+ `cinder_opcode_ids.h`（SP）。

## Outstanding Questions

**Resolve before v1 release / before enabling startup-init：**
- **安全 import signal provider。** Phase 0 C++ + gdb 已关闭分类 schema 红线，但没有关闭 `startup_phase` 来源：原始 `import_stack` frame/code metadata 遍历在 `jitVectorcall` 中 SIGSEGV，`module_initializing` 覆盖不足，`early_window` 不能单独作为默认来源。实现可先编码非 startup 分支，但 startup-init 分支、ImportInit 收益声明和 v1 release 必须先选择并验证安全 provider，再把它作为 `readGateContext().startup_phase` 的来源。
- **生产 policy/default 冻结。** `schema_freeze` 已有 Phase 0 C++ 证据；`policy/default_freeze` 仍需 A/B 与相邻配置比较，不能由 Mixed/family 红线直接推出。
- 已关闭：默认 bucket cutoff / floor / `loop_score` 嵌套阈值采用 bootstrap defaults + Phase 0 C++ gate-side dump 通过红线；`structure_key` 缓存无运行期失效，分类配置进程内冻结（T3.11）。

**Deferred to planning：**
- **Phase-3：** `specialization_band` 边界与滞回宽度、`specialization_presence` 重读频率（每次 gate 重扫特化位，还是缓存 + 每 N 次惰性刷新）。

### From 2026-06-02 ce-doc-review（待决，best-judgment 批处理后移交）

> 本轮 5 persona 审校（coherence/feasibility/product-lens/scope-guardian/adversarial）的判断/前提类发现。同步与缺省值类修复已直接回灌正文；以下 5 项是判断/前提题，需作者定夺。

- **[P1] v1 价值主张依赖未建成的 import provider（product-lens+adversarial）。** Problem Frame 以 startup/import compile-storm 立项，但唯一命中它的 `startup_init_candidate` 被未实现的安全 import signal provider 挡住，可 ship 的只剩 Trivial/synthetic/risk-defer。建议在 Summary/Scope 用“provider 缺席时 v1 实际交付什么”重述价值主张，使其可独立证伪。
  - **↳ [P2] startup-init 关闭时 storm-reduction A/B 不可测（product-lens）。** Release gate(2) 要求证明减少 candidate 编译，但最大 storm 源在 v1 关闭；应说明 provider 缺席下可达成的 storm 削减量级，或把该 gate 拆成“provider 前/后”两档。（随上条一并处理。）
- **[P2] Phase-3 设计是否移出 v1 接口工件（scope-guardian）。** `isSpecializableOpcode` 声明在 v1 公有接口、`Signature` 带 Phase-3 字段注释、详细 §11 含完整 Phase-3 C++ 伪代码。若有意保留为 forward-reference 可不动；若担心实现者混淆，建议把这些移入 Phase-3 段或独立 stub 文档。
- **[P2] `Mixed` 是否纳入 `startup_init_candidate`（scope-guardian）。** R12 说 startup-init 覆盖“主族偏 dynamic/dispatch/object/control”的无 loop/非 static 函数，但 §13.2 family 白名单不含 `Mixed`，符合条件的 Mixed 启动函数会漏判。二选一：(a) 把无 loop/非 static 的 Mixed 纳入；(b) 显式文档化排除并补一条 AE。
- **[P2] 静态签名弱预测 ROI 的核心前提（product-lens+adversarial）。** 文档自承 L1（静态≠执行构成）、L3（纯动态不可静态识别），且 loop_score 是唯一可靠热度信号；v1 对非 loop 族的抬阈值建立在未证明的相关性上，且 v1 内无反馈回路纠正误判。建议界定/量化误判率（mis-defer 上界），或在 Scope 明确“v1 不保证 ROI 预测精度，仅削减明确低收益形态”。

## 审校决策（ce-doc-review 2026-06-01，已定）

> 来自一轮四 persona 审校。T1（源码事实错误）与 T4（一致性/锚点）已在文档内直接修复；**T2（范围裁剪）** 与 **T3（前提/分期）** 经逐项决策，结论如下，并已回灌正文。

**T3 — 前提/分期（决定整体形态）：**
- **T3.1 ✅ 采纳 (b)：v1 带最小有用策略。** 默认策略不再是 no-op，而是对明确 `raise_threshold_candidate` 抬阈值：低 ROI 形态（`Trivial`、窄 synthetic 低 ROI：`is_synthetic && loop_score==0 && !is_static && family ∈ {ReflectionMeta, Trivial}`）、`startup_init_candidate`（startup/import 阶段可达的初始化形态函数）、以及受限的 `compile_risk_defer_candidate`（`high_risk && loop_score==0 && !is_static`）。其余族走现状阈值。这把 v1 从"零收益脚手架"变为"最小可用切片"，端到端验证管线。**KD2 的"只分类"边界相应放宽为"分类 + 最小阈值策略"。**
- **T3.2 ✅ Phase 0 C++ gate-side dump 已完成；startup/import signal 仍需补证。** C++ clean summary（`scratch/autojit_phase0/results/blue-98-20260602-cpp/summary-clean/summary.json`）跑过 pyperformance subset + django 专项，`Mixed(storm)=2.9%`、最大 storm family 未超过 50%、`gate_reachable=99.8%`，因此 family/Mixed schema、gate observation 口径和 bootstrap cutoff/floor/δ/loop defaults 可进入 v1 编码起点。它冻结的是 `schema_freeze`，不是 `policy/default_freeze`。红线未通过的部分是 startup/import signal：`module_initializing` 只覆盖 795/30605 个 storm；`import_stack` 原始 C++ frame 遍历由 gdb 定位为 SIGSEGV；`early_window` 只能作辅助。若 v1 继续宣称解决 ImportInit 收益目标，必须补实现安全 import signal provider 并复跑 dump。
- **T3.3 ✅ 定轻量标定协议。** (1) 标定语料用混合集（pyperformance + import/dispatch 密集的真实 workload，平衡目标按后者）；(2) cutoff/floor/δ/risk 比率做成**可按部署覆盖**（env/config），非编译期硬编码；(3) 文档记录 CPython 版本升级触发重标定；(4) policy/default freeze 必须跑 `PYTHONJITAUTO=auto[:N]` vs 数值 `N` A/B，并至少比较一组相邻 cutoff/floor/δ/loop 配置。与 T3.2/Phase 0.5 实验合并跑。

**T2 — 范围裁剪：**
- **T2.1 ✅ `AutoJitPolicy` 虚类降为自由函数** `computeThreshold(structure_key, gate_context, ...)`，内含 T3.1(b)/T3.4/T3.5/T3.6 最小策略；出现第二种策略再提升为接口（YAGNI）。
- **T2.2 ✅ v1 整体砍掉特化观测。** 因 T3.1(b)/T3.4/T3.5/T3.6 最小策略不读 `specialization_band`，整条旁路（`spec_band` 字段、`readSpecializationBand`、滞回、AE10）v1 无消费者，**整体 defer 到 Phase-3**，与其后继的计数式真稳定性信号一起做。`gate_view` 在 v1 简化为 `structure_key + gate_context`。
- **T2.3 ✅ 不新增环境变量，复用 `PYTHONJITAUTO`（扩展取值）。** 把 `PYTHONJITAUTO` 从纯数值改为可接受 `auto[:N]`（已核实 FlagProcessor 有 `void(const std::string&)` 重载，`jit_flag_processor.h:84`）：
  - `-X jit-auto`（空 X-option）→ 保留现状：阈值 1、**分类关**；注意空 env（`PYTHONJITAUTO=`）不等价于 1；
  - `PYTHONJITAUTO=<N>`（整数）→ 现状：固定阈值 N、**分类关**（不变，既有测试 `PYTHONJITAUTO=10` 不受影响）；
  - `PYTHONJITAUTO=auto` → **分类开**，base 阈值取默认；`PYTHONJITAUTO=auto:N` → 分类开、base=N。
  - malformed / negative / empty env / overflow → 记录 invalid，字段保持原值，不静默开启分类，也不静默转成阈值 1。
  - A/B 对照 / 热路径止血 = 把值改回数字（分类关）。语义比独立布尔开关更融入现有使用场景，且与 INVALID 回退（仅分类失败时）正交。
- **T2.4 ✅ 砍掉 `ScalarCompute` 族。** compute 主导一律归 `NumericLoop`，由 `loop_score` modifier 区分有无循环（`NumericLoop|loop0` 即原 ScalarCompute）。结合 T3.4 移除不可达 `ImportInit` 后，v1 family 枚举为 8 个。
- **T3.4 ✅ 修正 ImportInit 可达性。** startup/import compile storm 是本特性主收益目标之一，但当前 `ImportInit` 定义（缺 required flags / `<module>`）在 `jitVectorcall` gate 上不可达。文档改为：不可达初始化代码进入 Phase 0 `InitCodeDiagnostic` 诊断桶；v1 策略命中可达的 `startup_init_candidate`，通过 `gate_context.startup_phase` 参与 `computeThreshold`，但不并入 `structure_key`。
- **T3.5 ✅ `startup_phase` 来源先采样后冻结；C++ 原始 `import_stack` 实现禁止进入热路径。** 不在设计期把 `startup_phase` 绑定到单一信号。Phase 0 dump 同时采样 importlib/module initializing、安全 import 状态 provider、早期进程窗口等候选信号；早期进程窗口只作对照，不得单独成为默认策略来源。2026-06-02 gdb 证据证明，在 `jitVectorcall` 中遍历 Python frame/code metadata 计算 `import_stack` 不安全；正式实现必须改为 import machinery 侧轻量 depth/counter、thread-local import state 或等价安全来源。只有当候选信号覆盖主要可达 compile storm 且误伤可解释时，才冻结为 `readGateContext()` 的 bool 输入并接入热路径。
- **T3.6 ✅ `high_risk` 不等同低 ROI。** `risk` 是编译成本/安全信号，不再一刀切抬阈值。v1 抬阈值条件改为 `raise_threshold_candidate = low_roi_candidate || startup_init_candidate || compile_risk_defer_candidate`；其中 `low_roi_candidate = Trivial || (is_synthetic && loop_score==0 && !is_static && family ∈ {ReflectionMeta, Trivial})`，`compile_risk_defer_candidate = high_risk && loop_score==0 && !is_static`。为保持可实现性，synthetic/generated 从 `high_risk` 中拆出为独立 `is_synthetic` modifier；synthetic 高 loop/static/generated 与 risk-defer 的默认启用都必须有 ROI 证据。
- **T3.7 ✅ Mixed 保留 top-2 形态。** `Mixed` 仍是近并列兜底，但 v1 在 `structure_key` 中记录 canonical top-2 工作维度组合 `mixed_shape`（非 Mixed 为 `none`，最多 15 个子形态）。这避免 `Mixed` 聚合过粗，又不把完整 bucket tuple 扩散到所有 family。
- **T3.8 ✅ tie-break 改为 benefit-first。** `Mixed` 已承接近并列，剩余并列场景按 `compute > dispatch > object > control > dynamic > suspend` 选主族；风险/成本不再通过 tie-break 抢主族，而由 `is_suspendable`、`high_risk`、`is_synthetic`、`gate_context` 等 modifier/context 进入策略。
- **T3.9 ✅ 给 bootstrap defaults，并由 Phase 0 C++ gate-side dump 通过分类红线。** 文档给 `count_floor=2`、density cutoff `0.10/0.25/0.50`、Mixed bucket gap `<=1` 且双方 bucket `>=2`、`loop_score=max(nesting_score,count_score)`（backedge 数 `0/1/2–3/>=4` → `0/1/2/3`）作为 scanner 起跑值；2026-06-02 C++ clean summary 未触发 Mixed/family 红线，可作为 v1 编码起点。实现里这些常量集中配置并可按部署覆盖；生产默认值必须由 A/B 与相邻参数比较冻结。后续重新标定必须在新进程或清空 code objects 后进入 gate/cache/policy。
- **T3.10 ✅ `structure_key` 物理表示固定为 32-bit `skey_word`。** 逻辑 `StructureKey` 是聚合身份；物理缓存/发布使用 `uint32_t skey_word`（bit31 valid + 低 16 bit payload：`mixed_shape + family + modifiers`）。热路径、缓存、统计键使用紧凑整数或解码后的结构值；字符串只在 Phase 0 dump、日志和诊断中由 payload 解码生成，不作为缓存或聚合主表示。
- **T3.11 ✅ `structure_key` 缓存无运行期失效。** Phase 0 scanner/dump 可用 bootstrap/覆盖参数反复采样；一旦进入 gate/cache/policy，分类 schema/config（opcode 表、cutoff/floor/δ/loop 阈值、tie-break、payload 布局）在进程内冻结。`skey_word` valid 后不失效、不版本比对、不重算；配置变更需要新进程或清空 code objects。

## Sources / Research

- `cinderx/Jit/pyjit.cpp:183` `jitVectorcall`（阈值门 `:197`）—— AutoJIT 准入点，签名注入位置。
- `cinderx/Jit/pyjit.cpp:101` `countCalls`/`codeExtra` —— 既有 per-code-object 计数与缓存，结构核复用此处。
- `cinderx/Jit/pyjit.cpp:96` `required_code_flags`(`CO_OPTIMIZED|CO_NEWLOCALS`)；`getCompilationEligibility` 与 `compilePreloaderImpl` 均拒绝缺 flags code —— `InitCodeDiagnostic` / gate 可达性依据（R12/T3.4）。
- `cinderx/Jit/hir/builder.cpp` —— opcode 处理集合，R1–R6 证据来源（取其 3.14 分支）。
- `cinderx/Interpreter/3.14/cinder_opcode_ids.h` —— Static Python opcode 定义（`EXTENDED_OPCODE_FLAG=0x200`，SP opcode ≥512），R24/家族表 SP 子表依据。
- `cinderx/Jit/bytecode.cpp:106` 公有 `opcode()`（canonical + SP flag）、`:153` `specializedOpcode()` —— 归一（R22）与特化位读取（R11）；`uninstrumentedOpcode` 为 private，勿用（审校 T1.2）。
- `cinderx/Jit/bytecode.cpp:293` —— 证实自适应特化形态存在于 code object（`specialization_presence` 的依据）。
- `cinderx/Interpreter/3.14/Includes/ceval_macros.h`（`DEOPT_IF`、`backoff_counter`/`advance_backoff_counter`、`JUMP_TO_PREDICTED`）—— 证实特化路径 miss 时 guard+退避、特化形态原地保留，是 KD6/R11 把特化观测降级为弱信号的依据（审查 Finding 2）。
- `cinderx/Common/code_extra.h:12` `CodeExtra`、`:35`/`cinderx/Jit/config.h:157` `Py_GIL_DISABLED` 支持；`code_extra.h` 的 `calls` 访问器为 relaxed/seq_cst（非 release/acquire）。release/acquire 发布范式见 `cinderx/Jit/context.cpp:523`（`jit_compiled`）——R26/KD8 沿用此范式（审查 Finding 3，锚点经审校 T4.1 修正）。
- `cinderx/Jit/osr.cpp:327` / `osr.h:159` `collectBackedgeTargetOffsets` —— 仅返回 backedge target 偏移（去重、上限 16），**不含 source 端点**；`loop_score`（R7）改为在分类器单次扫描内就地收集端点（审校 T1.3）。运行期 backedge 计数（`osr.h` 的 `BackedgeEntry`）可作下游热度反馈来源，非本期 gating 输入。
- `cinderx/Jit/hir/preload.cpp:449`、`inliner.cpp:172` —— `CI_CO_STATICALLY_COMPILED`，`is_static`（R9）依据。
- `cinderx/Jit/hir/hir_stats.cpp`（`PYTHONJITDUMPHIRSTATS`）—— HIR 层"按函数计指令/类型"先例；下游反馈可用，**不用于 gating 签名**（KD1）。
- Phase 0 C++ evidence：`scratch/autojit_phase0/results/blue-98-20260602-cpp/report.md`、`summary-clean/summary.json`、`logs/autojit-phase0-gdb-debug-container-20260602-115858.log`、`logs/autojit-phase0-gdb-after-fix-20260602-120011.log`。
- GitHub issue: sisibeloved/cinderx#3《探索基于行为模式的自适应 AutoJIT 阈值策略》—— 需求母体。
