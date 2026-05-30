# 功能设计说明书：Generators JIT TreeIter 状态机优化

## 1 产品版本&密级

| 产品版本 | 密级 |
| -------- | ---- |
| V1.0     | 公开 |

## 2 拟制信息

| 角色   | 姓名        | 日期       |
| ------ | ----------- | ---------- |
| 拟制人 | Codex Agent | 2026-05-26 |
| 审核人 |             |            |
| 批准人 |             |            |

## 3 修订记录

| 版本 | 日期       | 修订人      | 修订内容 |
| ---- | ---------- | ----------- | -------- |
| V1.0 | 2026-05-26 | Codex Agent | 整合 `github/bench-cur-7c361dce-claudecode` 分支 generators JIT 优化文档，按当前 master 重写功能设计 |
| V1.1 | 2026-05-26 | Codex Agent | 按文档审校意见收紧 CPython yield-from 语义、deopt/lifecycle、栈溢出和生产发布边界 |
| V1.2 | 2026-05-26 | Codex Agent | 按二轮审校意见固定首版实验边界、heap-backed 状态栈、生产 gate 和里程碑拆分 |
| V1.3 | 2026-05-28 | Codex Agent | 修复二轮审查剩余 gated_auto，补齐状态清理、实验消费边界和异常/准入契约 |
| V1.4 | 2026-05-28 | Codex Agent | 明确生产协议操作采用精确 deopt/reify，将环检测和深度限制限定为原始 generator 语义，并确定生产准入五个纵切面 artifact |
| V1.5 | 2026-05-28 | Codex Agent | 修复文档审查意见，拆分实验/生产 gate，补齐 reify 可表示性、active-path/depth 不变量、artifact/workload schema 和实验协议 guard |
| V1.6 | 2026-05-29 | Codex Agent | 按 generators 性能根因分析重设 V2 方向：支持原始 truthiness guard、dict-backed split-dict 字段证明，并明确 21 个 pyperformance JIT 用例中的直接覆盖与可复用基础能力边界 |
| V1.7 | 2026-05-29 | Codex Agent | 修复 V2 审校意见：收紧 `generators` 硬 gate、实验/生产 rollout 边界、运行时 guard 和准入 artifact 信任边界 |

## 4 Keywords 关键词

CinderX, JIT, generator, yield from, TreeIter, HIR, LIR, GenDataFooter, 状态机, FieldAccessProof, split-dict, AArch64

## 5 Abstract 摘要

本文档定义基于当前 `master` 重新实现 generators JIT 优化的功能设计。旧分支 `github/bench-cur-7c361dce-claudecode` 中存在多份阶段性计划、研究、诊断和经验总结，内容覆盖 `OptimizedYieldFrom`、`InlineIter`、逃逸分析、状态机生成和 postalloc 调试。经整合后，本次重新实现只保留已证明有效的核心路径：针对语义可证明的树遍历生成器，在 JIT 编译期生成显式状态机，将递归生成器帧切换转换为 `GenDataFooter` 驱动的局部状态循环。

V1.6 进一步收紧实现方向：性能目标仍以 pyperformance `generators` 暴露的真实瓶颈为直接验收对象，但实现不得写成该 benchmark 的特殊分支。TreeIter matcher 应建立通用的字段访问证明能力，覆盖 slot/member 字段和当前 master 对 dict-backed heap object 生成的 split-dict fast path；空子树 guard 也不再限定为源码写作 `is not None`，只要原始 HIR guard 与 exact default truthiness 证明共同表明 `if child:` 等价于 `child is not None`，即可作为候选形态。

本文档不再把裸 `yield from self.left/right` 直接等同于空子树跳过。按 CPython 语义，`yield from None` 必须抛出 `TypeError`；状态机只有在原始源码/HIR 已包含可证明的 child 跳过 guard 时，才允许替换原始 yield-from 路径。编译期观察到某个样本 child 非空、是同一精确 Node 类型，不能替代原始控制流 guard。truthiness guard 必须额外证明 default truthiness，不能隐式吞掉用户 `__bool__` / `__len__`。

本文档不直接移植旧分支的大量非 generators 改动、实验脚本、Docker 配置和阶段性文档；后续实现应以本文档作为功能边界，以当前 `master` 的 HIR 表示和 generator runtime 为准。首版实现按实验显式启用落地：TreeIter 状态机只有在 `PYTHONJITTREEITERSTATEMACHINE=1`、JIT 开启、目标平台已验证且 matcher 准入证明完整时生成；生产默认启用必须等待 exact deopt/reify、协议/lifecycle 和性能 gate 闭合。准入 gate 只允许可证明的 `left/value/right` TreeIter 形态进入状态机；字段证明、exactness、layout、iterator identity、lifecycle、平台 codegen 和验收矩阵必须共同闭合。

## 6 List of abbreviations 缩略语清单

| 缩略语 | 英文全称 | 中文名 |
| ------ | -------- | ------ |
| CFG | Control Flow Graph | 控制流图 |
| DCE | Dead Code Elimination | 死代码消除 |
| DFX | Design for X | 面向可靠性、性能、安全等属性的设计 |
| HIR | High-level Intermediate Representation | 高层中间表示 |
| LIR | Low-level Intermediate Representation | 低层中间表示 |
| JIT | Just-In-Time Compilation | 即时编译 |
| SSA | Static Single Assignment | 静态单赋值形式 |

## 7 前言

### 7.1 文档整合范围

旧分支中与 generators 优化相关的文档分散在：

| 原分支文档类别 | 代表路径 | 整合结论 |
| -------------- | -------- | -------- |
| 设计草案 | `docs/superpowers/generators/specs/*.md`、`docs/design/yield-from-inline-codegen-design.md` | 保留问题背景、状态机结构和风险点 |
| 研究报告 | `docs/superpowers/generators/research/*.md` | 保留运行时瓶颈分析和状态机机会判断 |
| 决策记录 | `docs/superpowers/generators/decisions/*.md` | 保留 GenDataFooter 栈数组、保守副作用标注等结论 |
| 诊断报告 | `docs/superpowers/generators/diagnostics/*.md` | 保留性能口径、测试方式和踩坑证据 |
| 经验总结 | `docs/superpowers/generators/2026-03-30-tree-iter-state-machine-lessons-learned.md`、`2026-04-01-regalloc-investigation-report.md` | 保留最终实现经验和已知限制 |

### 7.2 关键证据

旧分支最终结论：

1. 单独的 `OptimizedYieldFrom` 和简化逃逸分析收益有限，阶段性报告显示逃逸分析本身约 0.6% 改进，性能主要受运行时函数调用和生成器帧切换限制。
2. `TreeIterStateMachinePass` 将树遍历生成器转换为显式状态机后，在 macOS ARM64 和 Linux AArch64 / kunpeng 均验证正确性。
3. Plan B 将状态读写、栈操作和 current node 读写从 C 运行时调用改为直接 codegen 后，旧分支记录了约 4-12x 加速，后续 postalloc 修复后部分场景达到约 15x。
4. 可靠性风险集中在引用计数、`InitialYield` 前后的寄存器 clobber、SSA 插入顺序、`hasArbitraryExecution` 标注、postalloc move fold 和 free-list 内存初始化。

### 7.3 当前 master 约束

当前 `master` 与旧分支的差异会影响重新实现方式：

| 领域 | 当前 master 状态 | 对重新实现的约束 |
| ---- | ---------------- | ---------------- |
| yield-from HIR | 通过 `Send` 和 `YieldValue::yieldFromIter()` 表示，未引入旧分支的显式 `YieldFrom` 指令 | 模式检测应优先适配当前 HIR，不把旧分支 HIR 重构作为前置条件 |
| Pass pipeline | 无 `TreeIterStateMachinePass` 配置位 | 新 pass 需新增独立配置并默认保守启用策略 |
| GenDataFooter | 只保存 generator resume 所需字段 | 首版只增加 TreeIter 状态指针；状态对象按需分配，避免给所有 JIT generator 内嵌大栈数组 |
| LIR/codegen | 已有 generator yield/resume 路径 | 状态机专用指令应最小接入现有 LIR 和 codegen 规则 |
| 测试 | 有 generator JIT 基础测试，但无 TreeIter 状态机回归 | 需要新增 correctness、触发探针和性能对比测试 |

# 8 功能域 Generators JIT 状态机优化

## 8.1 功能域概述

本功能域面向 CinderX JIT 对递归生成器的优化，首个实现目标只覆盖语义可证明的树遍历生成器模式。生产可优化形态必须把空子树跳过写入原始控制流。原始控制流可以是显式 `None` guard：

```python
def __iter__(self):
    if self.left is not None:
        yield from self.left
    yield self.value
    if self.right is not None:
        yield from self.right
```

也可以是原始 truthiness guard，但只有在 matcher 能证明非空 child 为同一 exact Node 类型，且该类型使用默认 truthiness、没有自定义 `__bool__` / `__len__` 时，才允许把它视为 `None` guard：

```python
def __iter__(self):
    if self.left:
        yield from self.left
    yield self.value
    if self.right:
        yield from self.right
```

未带 guard 的 `yield from self.left/right` 不是上述形态。若 `left/right` 为 `None`，CPython 会在 `GET_YIELD_FROM_ITER` / `PyObject_GetIter` 路径抛出 `TypeError`；状态机不得把裸 yield-from 改写为空遍历。若 truthiness guard 可能调用用户代码或把非 `None` child 判为 false，也不得优化。

该模式来自 pyperformance `generators` benchmark 和树结构遍历代码，但当前 master 是否能匹配正式 benchmark 不能只按源码形态推断。当前已知根因是正式 benchmark 使用 dict-backed heap object，HIR 中字段访问表现为 split-dict fast path 与 `LoadAttr` fallback 的合流，而不是单一 slot `LoadField`。因此 V2 必须以通用 `FieldAccessProof` 表达字段来源：slot/member 字段、split-dict inline-values 字段、对应 guard/dependency、失效或 deopt 处理。生产默认启用前必须提交当前 master 目标 `Tree.__iter__` 的 HIR 形态、truthiness 等价性、exactness/layout/iterator identity 证据，并在实现中保持失败即不改写的准入边界。原始 JIT 路径仍按 Python generator 语义维护递归生成器帧，每个 `yield from` 都可能触发子生成器创建、恢复入口查询、帧切换、`gi_yieldfrom` 暴露、`send/throw/close` 委派和状态保存。状态机优化将上述递归控制流转换为单个 JIT generator 内部的显式循环；任何无法证明语义保持的形态都拒绝优化：

```text
current node + phase + state stack
    -> check left
    -> yield value
    -> check right
    -> backtrack
```

功能边界：

| 范围 | 结论 |
| ---- | ---- |
| 树遍历生成器状态机 | 首版实验实现，默认关闭，可显式启用 |
| 泛化任意递归生成器 | 不涉及 |
| 消除所有 generator frame | 不涉及 |
| 逃逸分析驱动的 caller 内联 | 不作为首版依赖 |
| pyperformance 正式覆盖 | `generators` 是 V2 直接性能目标和实验收益硬 gate；实验覆盖需先给出当前 master split-dict/truthiness 实验准入证据；生产覆盖需五个纵切面准入 artifact verifier；若 `generators` 无法准入，V2 停止在 matcher 研究，不用替代 workload 宣告本功能成功 |

生产状态：

| 项目 | 结论 |
| ---- | ---- |
| 首版交付定位 | 显式启用的 TreeIter 实验优化，用于验证 `generators` 代表的递归树遍历负载 |
| 启用条件 | JIT 开启、目标架构已验证、目标 HIR 形态已匹配，且 HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt/lifecycle 证明全部满足 |
| 协议边界 | 状态机必须保持普通 `next()`/`for`/`list()`、`send(non-None)`、`close`/deopt/GC lifecycle 的既有语义；无法证明的 generator 形态保持原始路径 |
| 显式启用 | `PYTHONJITTREEITERSTATEMACHINE=1` 打开该 pass，用于 A/B、benchmark 和问题定位 |
| 不满足生产前置时 | 默认配置下 pass no-op；不得为证明不完整的函数生成状态机 |

### 8.1.1 pyperformance JIT 用例覆盖边界

21 个 pyperformance JIT 用例中，TreeIter 状态机的直接覆盖范围必须按行为模式定义，而不是按 benchmark 名称硬编码：

| 分类 | 用例 | 结论 |
| ---- | ---- | ---- |
| 直接目标 | `generators` | 递归对象树 + `yield from` + 原始 child guard，是本功能的直接性能验收对象；V2 必须支持其 dict-backed split-dict 字段访问和 truthiness guard 形态 |
| 相邻但不同的 generator 优化 | `nqueens`；`scimark_lu`/`scimark_fft` 中的非热路径 helper | `nqueens` 是普通 generator frame/state 形态，不是递归对象树 `yield from`；Scimark 核心热路径是数值循环和下标访问，helper `yield` 不代表 TreeIter 机会 |
| 可复用字段证明基础能力 | `richards`、`richards_super`、`deltablue`、`go`、`hexiom`、`raytrace`、`chaos`、`float`，以及可能的 `sqlglot_v2_parse`/`sqlglot_v2_transpile` 库内部 | 这些用例没有 TreeIter yield-from，但大量使用 exact heap object、slot 或 dict-backed 属性访问和方法分派。`FieldAccessProof`、exactness、layout dependency、deopt/invalidation 可作为后续 OO-heavy 优化的基础能力；V2 的实现验收仍只接受 TreeIter `left/right/value` 形态 |
| 本功能范围外 | `unpack_sequence`、`pickle_pure_python`、`unpickle_pure_python`、`nbody`、`regex_compile`、`spectral_norm`、`fannkuch`、`scimark_lu`、`scimark_fft` | 主要瓶颈在 sequence unpack、pickle/regex/sql parser 库逻辑、数值循环、列表/下标/切片和容器操作，不应由 TreeIter 状态机承诺收益 |

因此本轮验收不追求 21 个用例的广泛收益。M4 实验性能 gate 只围绕 `generators` 和明确的非目标 generator 回退样本；21 个 JIT 用例列表用于限定收益声明、抽样发现意外回归，以及 M5/default rollout 前的更宽回归矩阵。文档和实现必须保证 TreeIter 状态机直接服务 `generators` 所代表的递归树遍历形态，同时把字段证明设计成可复用 primitive，避免后续优化 OO-heavy 用例时重复发明一套 exact field/layout/deopt 机制。

## 8.2 功能域总体方案

总体方案分为六层：

```text
Python generator bytecode
        |
        v
当前 master HIR: guard + Send + YieldValue(yieldFromIter) + GET_YIELD_FROM_ITER CFG
        |
        v
TreeIterStateMachinePass
        |
        v
状态机 HIR: Ensure/ClearTreeIterState, Load/SaveCurrentNode, Load/SavePhase, StateStackPush/Pop, YieldValue
        |
        v
LIR + codegen: 通过 FP 正偏移读取 GenDataFooter.tree_iter_state 指针
        |
        v
运行时: GenDataFooter 分配、初始化、generator suspend/resume
```

设计原则：

1. 以当前 master 的 HIR 为输入，不要求先移植旧分支的 `YieldFrom`、`OptimizedYieldFrom`、`InlineIter` 全套指令。
2. `GenDataFooter` 只保存 TreeIter 状态指针，实际 current/phase/stack 以及生产 active-path/depth 辅助状态放在按需分配的 heap-backed `TreeIterState` 中，避免给所有 JIT generator 增加固定 256B+ 状态数组。
3. 新增 HIR 指令按副作用保守建模，写操作和引用计数相关操作不得被 DCE/CSE 错误消除。
4. 首版只匹配原始 child guard、exact type/layout、字段访问证明和 iterator identity 都明确的 `left/right/value` 树遍历；child guard 可以是 `is not None` 等价 guard，也可以是带 default truthiness 证明的 truthiness guard，任何不确定情况直接回退到原始 generator 路径。
5. 首版 native codegen 只保障 AArch64 路径；x86_64 首版只实现 no-op arch gate，不生成状态机机器码，后续独立完成同等 correctness、protocol、deopt 和性能矩阵后再加入 native translate 规则。

## 8.3 功能域规格设计

### 8.3.1 功能规格

| 编号 | 规格 |
| ---- | ---- |
| GJIT-SM-001 | JIT 编译树遍历生成器时，只识别原始源码/HIR 已显式包含空子树跳过语义的 `left/value/right` 中序遍历模式；该语义可以来自 `is not None` 等价 guard，或来自 truthiness guard 加 exact default truthiness 证明 |
| GJIT-SM-002 | 生产默认启用的状态机必须保持 CPython 可观察 generator 语义与原始 generator 完全一致，包括异常、`gi_yieldfrom`、`send/throw/close`、StopIteration value 和 suspend/resume 状态；无法证明时不得改写 |
| GJIT-SM-003 | 状态栈采用 heap-backed growable 结构；release 构建每次 push 前必须动态检查并安全扩容或报错，不能只依赖 debug 断言 |
| GJIT-SM-004 | 状态机路径不得把裸 `yield from None`、非 iterable、子类覆盖或自定义 iterator 协议改写为普通空遍历 |
| GJIT-SM-005 | 状态机相关 `PyObject*` 必须满足 CinderX RefcountInsertion、GC traverse、clear/dealloc、deopt 和 generator finalize 的所有权假设 |
| GJIT-SM-006 | 功能必须可通过独立 JIT 配置关闭，用于回归定位和性能对照 |
| GJIT-SM-007 | 目标架构未完成 codegen 和测试矩阵前，即使配置开启也必须 no-op |
| GJIT-SM-008 | 生产默认启用必须具备当前 master 五个纵切面准入证明、量化性能 gate 和完整协议回归 gate |
| GJIT-SM-009 | 字段访问必须通过通用 `FieldAccessProof` 表达，覆盖 slot/member 与 dict-backed split-dict fast path；证明对象应可被后续 OO-heavy 优化复用，不能写成 pyperformance `generators` 专用分支 |

### 8.3.2 非目标

| 编号 | 非目标 |
| ---- | ------ |
| GJIT-SM-N001 | 不重写 CPython generator 协议；无法通过 exact deopt/reify 保持协议时拒绝生产优化 |
| GJIT-SM-N002 | 不在首版支持任意属性名、任意递归图、任意子类或用户自定义 iterator 协议内联 |
| GJIT-SM-N003 | 不把旧分支中与 ARM benchmark、Docker、wheel、其它 HIR 优化相关的改动纳入本功能 |
| GJIT-SM-N004 | 不承诺首版实验路径支持 deopt 后恢复到状态机精确位置；生产路径在精确 reify 未实现前必须 no-op |

## 8.4 功能项 TreeIter 状态机优化

### 8.4.1 功能概述

该功能项在 JIT HIR 优化阶段识别目标树遍历生成器，并用显式状态机替换原有递归 `yield from` 控制流。生成后的状态机通过 `GenDataFooter` 指针引用按需分配的 `TreeIterState` 保存：

| 状态字段 | 说明 |
| -------- | ---- |
| `current_node` | 当前遍历节点 |
| `current_phase` | 当前阶段：left、yield、right、backtrack；生产 active-path/depth 路径可使用 exit cleanup marker |
| `stack_capacity` | 当前 heap stack 容量 |
| `stack_top` | 显式遍历栈栈顶 |
| `state_stack` | heap-backed growable 栈，保存待回溯的 `(node, phase)`；生产路径可用 exit marker 表示 right 子树完成后还需移除 parent active-path |
| `popped_phase` | 单输出 HIR 限制下保存 `StateStackPop` 的 phase 结果 |
| `depth` | 生产路径中当前递归语义深度 |
| `depth_budget` | 生产路径中原始递归 generator 可接受的深度预算 |
| `active_path` | 生产路径中当前递归路径的节点身份集合，用于检测回边而不做全图 visited 去重 |

### 8.4.2 实现思路

原始逻辑必须已包含空子树跳过 guard；若无 guard，即使编译期样本显示 child 非 None、同一 exact Node iterator 且未触发自定义 iterator 协议，也不得匹配。guard 来源分两类：

| guard 类型 | 匹配要求 |
| ---------- | -------- |
| `is not None` 等价 guard | HIR 中能追踪到原始 `None` 判定和 yield-from 分支支配关系 |
| truthiness guard | HIR 中能追踪到原始 `if child:` 判定，并证明 child 只可能是 `None` 或同一 exact Node 类型，且 Node 类型使用默认 truthiness、没有自定义 `__bool__` / `__len__` |

```text
Node.__iter__
  -> if left is not None: yield from left generator
  -> yield value
  -> if right is not None: yield from right generator
```

目标逻辑：

```text
init:
  current = self
  phase = LEFT

loop:
  switch phase

LEFT:
  left = load current.left through FieldAccessProof
  if replayed original left guard says no child:
      phase = YIELD
  elif dynamic exact type / iterator identity guards accept left:
      push(current, YIELD)
      current = left
      phase = LEFT
  else:
      runtime failure action for the enabled layer

YIELD:
  yield current.value
  phase = RIGHT

RIGHT:
  right = load current.right through FieldAccessProof
  if replayed original right guard says no child:
      phase = BACKTRACK
  elif dynamic exact type / iterator identity guards accept right:
      current = right
      phase = LEFT
  else:
      runtime failure action for the enabled layer

BACKTRACK:
  if stack is empty:
      return None
  current, phase = pop()
```

首版目标逻辑不包含 active-state Python attribute fallback。matcher 的静态证明只允许生成后续动态 guard，不能替代每次字段读取后的 exact type、iterator identity 和 layout 检查。无法在 matcher 阶段证明 child guard、field access、layout 和 iterator identity 的函数，必须在 CFG 改写前保持原 HIR 不变。dict-backed 对象的 split-dict fast path 不能被当作普通 `LoadField` 特例处理，必须先形成 `FieldAccessProof`，并在状态机字段读取时保留对应 valid/layout guard 与失效/deopt 处理。

该设计实质上把递归调用栈替换为显式栈，把生成器恢复点替换为 `current_phase`。显式栈首版使用初始容量 16 的 heap-backed growable 结构。release 路径必须在每次 push 前做容量检查；容量不足时调用 grow helper 扩容，扩容失败时走受控错误路径，不能越界写或只依赖 debug 断言。

### 8.4.3 实现设计

#### 8.4.3.1 模式识别设计

准入条件：

| 条件 | 要求 |
| ---- | ---- |
| 代码对象 | generator function，且字段名集合包含 `left`、`right`、`value` |
| HIR 形态 | 当前 master 中应从 `YieldValue::isYieldFrom()` 或对应 `Send` 链路找到 yield-from iterator，并完整识别 `GET_YIELD_FROM_ITER` 生成的 coroutine rejection、exact generator assign、slow-path `GetIter` 和 merge 结构 |
| 空子树语义 | 必须在原始源码/HIR 中存在 child 跳过 guard；`is not None` guard 可直接使用，truthiness guard 只有在 default truthiness 证明成立时才能视为 `None` guard；不得给裸 `yield from self.left/right` 人为增加 `None` 为空的语义 |
| 字段来源 | yield-from iterator 来源必须能追溯到 `self.left` 或 `self.right` 的 `FieldAccessProof`；证明可以来自 slot/member `LoadField`，也可以来自 split-dict inline-values fast path 与 `LoadAttr` fallback 合流，但必须携带 valid/layout guard 和失效/deopt 处理 |
| iterator identity | child 的 `__iter__` 必须解析到当前被优化的 code object；若存在子类覆盖、descriptor/property 副作用或自定义 iterator 协议，必须回退 |
| 产出值 | 普通 `yield` 值必须能追溯到同一精确节点布局的 `value` 字段访问证明 |
| 控制流 | 仅允许匹配目标树遍历骨架；出现额外副作用、不明调用、异常处理复杂路径或无法精确 deopt 的节点时回退 |

检测失败必须保持原 HIR 不变。

`FieldAccessProof` 是本功能对其它 OO-heavy 优化可复用的关键输出，逻辑字段如下：

| 字段 | 说明 |
| ---- | ---- |
| `kind` | `slot_or_member` 或 `split_dict` |
| `owner_type` | 字段所属 exact heap/static type，以及对应 type-version/layout dependency；普通 heap class 通过动态 `CheckExact`/type-version guard 证明，不因可被子类化而直接排除 |
| `field_name` | `left`、`right` 或 `value`，后续泛化可用于任意 exact 字段 |
| `value_offset` | 可直接读取字段值的 offset；split-dict 场景为 inline values 中的字段 offset |
| `valid_offset` / `guard_source` | 证明 fast path 当前有效的 offset 与 guard 来源；slot/member 可为空，split-dict 必须记录 inline-values valid/layout guard |
| `fallback_shape` | 原始 HIR 中 slow path 的 `LoadAttr`/fallback 形态，用于证明语义和生产 deopt/reify；状态机不得静默删除可观察 fallback |
| `runtime_failure_action` | 已生成状态机后，guard 失败、layout 变化或 descriptor/property 失效时的实验 fail-closed、invalidate 或 exact deopt/reify 处理；`no-match` 只属于 matcher 阶段，不能作为运行时动作 |

首版 TreeIter matcher 可以在 pass 内部持有该结构，但语义上它不是 `generators` 特例：后续 `richards`、`deltablue`、`go`、`hexiom`、`raytrace`、`chaos`、`float` 等 OO-heavy 用例需要的字段 exactness、layout dependency 和失效处理，应尽量复用同一证明模型。

#### 8.4.3.2 状态存储设计

状态机不在 HIR SSA 中创建循环 Phi。原因是旧分支证明 Phi 自引用与后续 CopyPropagation、类型重推导、寄存器分配组合风险较高。`GenDataFooter` 只新增一个稳定 offset 的 `TreeIterState*`，实际状态放在按需分配的 heap 对象中。

逻辑数据结构：

```text
GenDataFooter
  tree_iter_state: TreeIterState*

TreeIterState
  current_node: PyObject*
  current_phase: int32  # left/yield/right/backtrack，生产路径可含 exit cleanup marker
  stack_top: int32
  stack_capacity: int32
  popped_phase: int32
  depth: int32
  depth_budget: int32
  active_path: identity set
  state_stack:
    node: PyObject*
    phase: int32
```

容量策略：

| 项目 | 首版规格 |
| ---- | -------- |
| 初始栈条目数 | 16 |
| 单条目大小 | 16 bytes |
| 支持深度 | 核心 correctness 覆盖 depth 1-12；生产路径只支持原始递归 generator 语义可接受的深度范围，不因 heap stack 动态扩容而隐式支持更深的树 |
| 超限行为 | release 构建每次 push 前同时检查物理容量和语义深度预算；容量不足但仍在预算内时扩容，分配失败走受控错误路径；若继续进入 child 将超过原始递归语义边界，生产路径必须精确 deopt/reify，无法 reify 时不得生产优化 |

生产路径不做全图 `visited` 去重。状态机只维护当前递归路径的 active-path 集合：进入 `left/right` child 前检查 child 是否已在当前路径中；命中表示存在回边，必须精确 deopt/reify 到原始 generator/yield-from 状态，让 CPython/CinderX 原路径产生对应行为。shared subtree 在离开当前路径后可以再次进入，不能因为“曾经见过”而跳过。

#### 8.4.3.3 CFG 生成设计

状态机 CFG 包含以下逻辑块：

```text
bb_init
  -> bb_loop
  -> bb_left / bb_check_yield / bb_check_right / bb_backtrack
  -> bb_has_left / bb_no_left
  -> bb_yield / bb_after_yield
  -> bb_has_right / bb_no_right
  -> bb_pop / bb_done
```

关键约束：

1. `EnsureTreeIterState()`、`SaveCurrentNode(self)` 和 `SavePhase(LEFT)` 必须插入到 `InitialYield` 之前，避免 `InitialYield` 后 caller-saved register 被覆盖；`EnsureTreeIterState()` 必须携带可复用 `FrameState`。
2. 新插入指令必须满足定义先于使用，不能用追加到块末尾的辅助函数在 `InitialYield` 前创建依赖。
3. `YieldValue` 必须携带原始 `FrameState`；如果无法安全构造 FrameState，则回退。
4. 状态机生成后必须重新清理不可达块，并对新寄存器做类型重推导，避免 `TTop` 被误当作 object 导致错误 decref。

#### 8.4.3.4 HIR/LIR/codegen 支持设计

新增状态机专用逻辑指令：

| 指令 | 类型 | 语义 | 副作用策略 |
| ---- | ---- | ---- | ---------- |
| `EnsureTreeIterState()` | HIR | 在首个状态写入前分配并初始化 `TreeIterState`，失败时按原 `FrameState` 抛出受控异常 | 可分配内存，必须保守 |
| `SaveCurrentNode(node)` | HIR | 更新 `TreeIterState.current_node`；输入可能来自 borrowed field load，必须先持有新 node 再释放旧 node | 写状态并处理 refcount/decref，必须保守 |
| `LoadCurrentNode()` | HIR | 读取当前节点 | 返回 object，需要给寄存器独立引用，必须保守 |
| `SavePhase(phase)` | HIR | 更新 `TreeIterState.current_phase` | 写状态，必须保守 |
| `LoadPhase()` | HIR | 读取 `current_phase` | 纯读，可在确认安全后放宽 |
| `StateStackPush(node, phase)` | HIR | 入栈并持有 node 引用，容量不足时扩容 heap stack | 写状态并处理 refcount，必须保守 |
| `StateStackPop()` | HIR | 出栈 node，phase 写到 `popped_phase` | 写状态并转移引用，必须保守 |
| `LoadPoppedPhase()` | HIR | 读取最近 pop 的 phase | 纯读，可在确认安全后放宽 |
| `LoadStackTop()` | HIR | 读取栈顶 | 纯读，可在确认安全后放宽 |
| `LoadTreeIterField(node, proof)` | HIR | 按 `FieldAccessProof` 读取 `left/right/value`；slot/member 走直接 load，split-dict 先验证 inline-values valid/layout，再读取字段 | guard 失败可能触发 invalidate 或 exact deopt/reify，生产路径必须保守；实验路径只能在已声明 harness 边界内使用 |
| `ClearTreeIterState()` | HIR | 正常 done、frame clear 和 dealloc backstop 清理 current/stack/active-path owned references | 写状态并处理 refcount/decref，必须保守 |

生产协议约束：

| 项目 | 约束 |
| ---- | ---- |
| `gi_yieldfrom` | 如果优化后 generator 在原语义中应处于 delegated yield-from 状态，生产路径必须在可观察前精确 deopt/reify 到等价原始 generator/yield-from 状态 |
| `send(non-None)` | 生产路径必须在发送前精确 deopt/reify，并交给 CPython yield-from 语义委派到当前子 iterator；无法 reify 时不得生产优化 |
| `throw/close/finalize` | 生产路径必须先精确 deopt/reify，再由原路径传播到当前 delegated iterator；无法 reify 时不得生产优化 |
| StopIteration value | 必须保留 yield-from 完成值处理；目标 TreeIter 不使用该值也不能改变异常传播 |

codegen 分层：

| 阶段 | 首版要求 |
| ---- | -------- |
| HIR | 定义 op、类型、printer、parser 需要的最小支持 |
| instr effects | 写操作和 refcount 操作默认 `hasArbitraryExecution=true`；纯读操作可在测试覆盖后优化 |
| LIR lowering | 状态机热路径使用原生 LIR opcode，不使用 `appendCallInstruction` 伪装成 C 调用；字段读取 lowering 必须消费 `FieldAccessProof`，不能把 split-dict 误降级为裸 offset load |
| AArch64 codegen | 通过 frame pointer 正偏移读取 `GenDataFooter.tree_iter_state`，再访问 heap state 字段 |
| x86_64 codegen | 首版只要求 no-op arch gate；native translate 规则后续单独实现和验证 |

#### 8.4.3.5 引用计数设计

所有状态机中保存的 `PyObject*` 必须明确所有权：

| 操作 | 引用计数规则 |
| ---- | ------------ |
| 保存 current node | 对新节点 `INCREF`，对旧节点 `DECREF` |
| 读取 current node | 返回前 `INCREF`，交给 RefcountInsertion 后续 `XDecref` |
| stack push | 栈持有节点引用，push 时 `INCREF` |
| stack pop | 栈引用转移给输出寄存器，pop 后清空槽位 |
| generator data 释放 | 清理 `TreeIterState.current_node`、栈内残留引用、生产 active-path 中 owned references 和 heap stack；depth/depth_budget 清零 |

该规则来自旧分支 depth>=3 崩溃的根因：运行时函数返回 borrowed reference 后被 RefcountInsertion 自动 `XDecref`，导致树节点被提前释放。

#### 8.4.3.6 配置与观测设计

| 项目 | 设计 |
| ---- | ---- |
| 配置项 | 新增独立 HIR optimization 配置，例如 `tree_iter_state_machine` |
| 环境变量 | 提供 `PYTHONJITTREEITERSTATEMACHINE` 开关 |
| 默认值 | 默认关闭；`PYTHONJITTREEITERSTATEMACHINE=1` 显式启用 |
| 触发探针 | 提供 debug/test-only counter，用于确认 pass 是否触发 |
| 日志 | 使用 JIT debug 日志，不向 stdout/stderr 输出热路径日志 |

### 8.4.4 增量SR清单

| 里程碑 | SR编号 | 需求描述 | 是否允许生成状态机 HIR |
| ------ | ------ | -------- | ------------------------ |
| M0 | SR-GJIT-001 | 新增配置、触发探针和 no-op pass 框架，保留显式关闭能力 | 否 |
| M1 | SR-GJIT-002 | 模式识别、当前 master HIR 形态、truthiness 等价性、`FieldAccessProof`、exactness/layout/iterator identity 实验证据、负例覆盖，失败时无行为变化 | 否 |
| M1.5 | SR-GJIT-002A | `generators` go/no-go：证明 pyperformance `generators` 当前 master 的 split-dict/truthiness HIR 形态满足实验准入；若不满足，V2 停止在 matcher 研究，不进入 M2-M4。非合成、生产等价的 TreeIter workload 只能作为后续 pivot 候选，必须另行决策后才能替代本 V2 目标 | 否 |
| M1.6 | SR-GJIT-002B | 生产可表示性 go/no-go：提交 exact reify 可表示性 artifact、active-path/depth 转移不变量草案、五纵切面 artifact schema 和 `generators` workload 接受标准；若可表示性失败，必须在 M2 前调整状态模型或明确 M2-M4 只保留实验目标 | 否 |
| M2 | SR-GJIT-003 | heap-backed `TreeIterState`、引用清理、GC traverse、release 扩容安全路径 | 否 |
| M3 | SR-GJIT-004 | 显式启用配置下生成 TreeIter 状态机 CFG、HIR/LIR/codegen 和 correctness 测试；默认配置必须 no-op | 是，实验配置 |
| M4 | SR-GJIT-005 | AArch64 性能 gate 和非目标 generator 回退检查 | 是，实验配置 |
| M5 | SR-GJIT-006 | 后续 rollout 增强：扩大协议/平台/形态覆盖、五纵切面 verifier 工具化、lifecycle 和生产性能 gate 持续闭合 | 是，生产配置 |
| 后续演进 | SR-GJIT-F001 | 泛化到更多递归生成器模式 | 否，不纳入首版 SR |

V2 实现范围到 M4 为止。M5 是生产默认启用前的增强候选；如果 M4 未证明 `generators` 在当前 master 上有稳定收益，生产默认启用策略必须回退或重新决策。

### 8.4.5 实现接口设计

#### 8.4.5.1 实现接口设计

模块间逻辑接口：

```text
Compiler pass pipeline
  -> TreeIterStateMachinePass.run(function)
     -> pass-local matcher(function)
     -> FieldAccessProof extractor(function, field name)
     -> pass-local state-machine builder(function, field proofs, frame state)
     -> TreeIterState pointer ops
     -> LIR/codegen lowering
```

接口约束：

1. Pass 输入输出均为 HIR `Function`，不得依赖 Python 层测试脚本。
2. TreeIter matcher 是 pass-local helper，不修改 HIR；`FieldAccessProof` 的语义边界按可复用 primitive 设计，首版可以在 pass 内落地，后续可提升为共享 helper。
3. builder 是 pass-local helper，只有完整匹配后才允许改写 CFG；生成失败必须可放弃改写，不能留下半状态机 CFG。
4. `GenDataFooter` 只暴露稳定的 `TreeIterState*` offset，状态对象分配和清理路径统一初始化。
5. LIR/codegen 不得 clobber allocator 可分配寄存器中的活跃值；新增原生指令必须纳入 postalloc 优化边界测试。

#### 8.4.5.2 实现接口定义

| 接口 | 输入 | 输出 | 说明 |
| ---- | ---- | ---- | ---- |
| `TreeIterStateMachinePass` | HIR function | HIR function 或不变 | 编译期模式识别与 CFG 改写入口 |
| pass-local matcher | HIR function | 匹配结果：字段证明、yield frame state、self register | 只服务 TreeIterStateMachinePass，不引入公共 PatternDetector 抽象 |
| `FieldAccessProof` extractor | HIR function、base register、field name | slot/member 或 split-dict 字段访问证明 | 首版供 TreeIter 使用，语义上可被后续 exact OO field 优化复用 |
| pass-local builder | 匹配结果 | 新状态机基本块集合 | 只负责本 pass 的 CFG 构造和原始路径替换 |
| TreeIter state HIR ops | node/phase/stack 操作 | 状态读写 HIR | 访问 `GenDataFooter.tree_iter_state` 指向的 heap state |
| LIR translate rules | 状态机 LIR op | 目标架构机器码 | 首版接入 AArch64 codegen，不创建独立 StateMachineCodegen 框架；x86_64 只保留 no-op arch gate |
| `JIT config` | 环境变量或配置对象 | pass enable/disable | 控制功能开关 |

### 8.4.6 功能规格设计

#### 8.4.6.1 正常流程

```text
1. HIR builder 构建当前 master 的 generator HIR。
2. 常规 simplify / cleanup 后进入 TreeIterStateMachinePass。
3. pass 确认 function 是带原始 child 跳过 guard、truthiness 等价性、exact type/layout、字段访问证明和 iterator identity 的目标树遍历 generator。
4. pass 提取 left/right/value 的 `FieldAccessProof`、self register、原始 guard、YieldValue FrameState 和实验/生产 gate 能力。
5. pass 在 InitialYield 前保存初始 current node 和 phase。
6. pass split 原入口块并生成状态机 CFG。
7. 状态机执行中通过 `GenDataFooter.tree_iter_state` 指向的 heap state 保存 current node、phase 和显式栈。
8. 每次 YIELD 阶段读取 current.value；首版实验只声明受控 `next()`/`for`/`list()` 消费结果正确，生产路径在协议敏感操作无法 exact deopt/reify 时不触发优化。
9. 栈为空且 phase 为 backtrack 时清理 tree_iter 状态并返回 None，generator 正常结束。
```

#### 8.4.6.2 异常与回退流程

| 场景 | 行为 |
| ---- | ---- |
| 字段名、字段访问证明或 HIR 链路不匹配 | 不改写，走原始 generator |
| truthiness guard 无法证明等价于 `is not None` | 不改写，走原始 generator，保留用户 `__bool__` / `__len__` 语义 |
| 缺少可复用 FrameState | 不改写，走原始 generator |
| split-dict valid/layout guard 失败 | 生产配置必须 invalidate 或 exact deopt/reify；无法 reify 时不改写 |
| TreeIterState 分配失败 | `EnsureTreeIterState` 在状态写入前抛出受控异常，不能留下半初始化状态 |
| 状态栈容量不足 | release 必须在写入前动态检查并在语义深度预算内扩容 heap stack；扩容失败走受控错误路径 |
| 检测到 active-path 回边或超过原始递归深度边界 | 生产配置必须精确 deopt/reify 到原始 generator；无法 reify 时不改写，走原始 generator |
| `yield from None` 或非 iterable | 保留 CPython 异常语义，不得作为空子树跳过 |
| 协议敏感操作无法 exact deopt/reify | 生产配置在 `send/throw/close/gi_yieldfrom` 前保持等价语义；无法证明时不改写，走原始 generator |
| refcount 或 dealloc 罕见路径 | codegen 需调用安全 dealloc 或退回 C helper |
| postalloc 优化破坏 move 链 | 新指令必须成为优化边界或补充中间寄存器使用检查 |
| 目标架构未验证 | 配置关闭或仅启用解释/原始 JIT 路径 |

#### 8.4.6.3 验收规格

| 类型 | 规格 |
| ---- | ---- |
| 正确性 | depth 1-12 完整中序遍历结果与 Python 原始实现一致，且裸 `yield from None`、非 iterable、显式 `None` guard、truthiness guard + default truthiness、truthiness guard + 自定义 truthiness 负例分别正确 |
| 稳定性 | 同一棵树重复遍历，多次 generator 创建和销毁无崩溃、无悬挂引用 |
| 准入 | 非 TreeIter generator 不触发 pass |
| 可控性 | 环境变量关闭时 HIR 不含状态机专用指令 |
| 协议 | 生产启用前 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、StopIteration value、异常上下文必须与 CPython 原语义一致；首版实验只覆盖受控 `next()`/`for`/`list()` 消费结果 |
| 环与深度 | finite acyclic tree 在原始递归语义可接受深度内结果一致；active-path 回边、self-cycle、right-cycle、极深 skewed tree 超过原语义边界时，生产路径精确 deopt/reify 或不触发优化，不能 silently 成功遍历 |
| 性能 | 指定平台和命令下，pyperformance `generators` 相对当前 master + feature off 达到量化收益阈值；M4 覆盖非目标 generator 抽样回退，M5/default rollout 前再覆盖 21 个 JIT 用例整体回归 |
| 回归 | 现有 `test_jit_generators`、`test_jit_coroutines`、generator frame、instrumentation deopt 相关测试通过 |

### 8.4.7 DFX分析

#### 8.4.7.1 可靠性分析

核心可靠性风险：

| 风险 | 设计约束 |
| ---- | -------- |
| 引用计数错误 | 明确 current node 和 stack node 的持有/转移规则 |
| 状态未初始化 | 所有 GenDataFooter 分配和 free-list 复用路径必须显式清零 |
| `InitialYield` 后寄存器被 clobber | 初始状态保存必须位于 `InitialYield` 前 |
| SSA 违规 | 插入到已有指令前的依赖链必须整体插入到同一位置前 |
| HIR 优化错误删除状态写 | 写状态和 refcount 指令保守标记副作用 |
| postalloc fold 跨越原生指令 | 新 LIR 指令必须参与寄存器使用和 clobber 分析 |
| CPython generator 协议漂移 | 生产启用前必须证明 `yield from` 可观察协议等价；不能证明时生产配置不触发 |
| release 栈越界 | push 前动态检查容量，容量不足扩容 heap stack，扩容失败走受控错误路径 |
| dict-backed 字段误判 | split-dict fast path 必须经 `FieldAccessProof`、valid/layout guard 和失效/deopt 处理；不得把 `LoadAttr` fallback 合流当作裸 `LoadField` 删除 |

##### 8.4.7.1.1 FMEA分析

| 失效模式 | 影响 | 原因 | 检测方式 | 预防措施 |
| -------- | ---- | ---- | -------- | -------- |
| 提前释放节点 | SIGSEGV 或错误遍历 | 返回 borrowed ref 后被 XDecref | depth>=3、多次遍历、ASAN | Load 返回前 INCREF，Save/Push 持有引用 |
| 状态机只遍历左叶子 | 结果错误 | SavePhase 被优化器消除或 LoadPhase 被 CSE | depth 1-12 正确性测试 | 写操作保守副作用 |
| Linux 偶发崩溃 | 非确定性 SIGSEGV | free-list 复用未清零状态字段 | 多轮 kunpeng 稳定性测试 | 分配路径显式初始化 |
| 编译后寄存器读垃圾 | 崩溃或错误节点 | InitialYield clobber self register | HIR dump 检查 init 顺序 | SaveCurrentNode 位于 InitialYield 前 |
| postalloc 删除返回值 move | 错误 decref 或 stale register | fold 未检查中间使用 | LIR dump 对比 | 增加优化屏障和中间使用检查 |
| `yield from None` 被跳过 | 本应抛出的 TypeError 消失 | 把 `None` 当空子树而非原始 guard | None/非 iterable 语义测试 | 只匹配原始 guard，裸 yield-from 保留异常 |
| `if child:` 语义漂移 | 自定义 false-y child 被错误遍历或跳过 | 未证明 default truthiness 就把 truthiness guard 等同于 `is not None` | 自定义 `__bool__` / `__len__` 负例测试 | truthiness guard 必须有 exact default truthiness 证明 |
| split-dict fallback 被删除 | 字段缺失、layout 变化或 descriptor 语义错误 | matcher 只追踪 fast `LoadField`，未保留 fallback/失效路径 | split-dict invalidation、字段删除、descriptor/property 负例 | matcher 可 no-match；已生成状态机后 `FieldAccessProof.runtime_failure_action` 必须为实验 fail-closed、invalidate 或 exact deopt/reify |
| close/throw 未委派 | generator finalization 或异常传播错误 | 普通 YieldValue 丢失 delegated iterator 元数据 | 生产 gate 协议测试 | 首版实验不声明支持；生产启用前必须在协议敏感操作前精确 deopt/reify |

#### 8.4.7.2 可服务性分析

1. 提供 pass 触发计数器或测试 hook，用于验证优化是否真正执行。
2. HIR printer 输出状态机专用指令，便于 `PYTHONJITDUMPHIR` 诊断。
3. 配置项可关闭状态机，支持 ON/OFF 性能和正确性二分。
4. 文档保留旧分支经验结论，后续 debug 时优先检查 refcount、init 顺序、副作用标注和 postalloc。

#### 8.4.7.3 安全设计检查

##### 8.4.7.3.1 安全设计确认

本功能不新增外部输入解析、网络访问、文件访问或权限边界。主要安全风险是 native code 内存安全：

| 项目 | 结论 |
| ---- | ---- |
| 对象生命周期 | 通过 refcount 规则约束 |
| 数组边界 | heap-backed `state_stack` 初始容量为 16，release 构建必须在每次 push 前动态检查并扩容；编译期和 debug 检查只作为补充诊断 |
| 未初始化内存 | `GenDataFooter.tree_iter_state` 必须初始化为空；heap state 分配后必须清零 |
| 架构差异 | 每个架构独立验证后启用 |

##### 8.4.7.3.2 敏感操作检查

不涉及权限、凭据或用户数据持久化操作。

#### 8.4.7.4 可用性/性能分析

性能目标来自旧分支最终经验，不作为本文档直接验证结论：

| 阶段 | 旧分支结论 | 本次设计取舍 |
| ---- | ---------- | ------------ |
| OptimizedYieldFrom | 约 1% 级别 | 不作为首版核心 |
| 逃逸分析/InlineIter | 单独收益有限 | 不作为状态机依赖 |
| 状态机 + C helper | 可正确但过慢 | 不作为最终热路径 |
| 状态机 + 原生 codegen | 旧分支记录 4-12x，后续部分场景约 15x | 首版目标路径 |

性能验证口径必须区分，并写入量化 gate：

| 口径 | 用途 |
| ---- | ---- |
| 当前 master JIT 关闭状态机 | 提交基线 |
| 当前 master + 状态机 | 改动后结果 |
| stock CPython | 口径基线 |
| CinderX JIT with jitlist | 避免低 auto 阈值编译 stdlib 干扰 |

生产默认启用前至少满足：

| Gate | 要求 |
| ---- | ---- |
| HIR 准入 | 当前 master HIR 形态、truthiness 等价性、exactness/layout/iterator identity 证据和接受标准全部通过 |
| 生产准入 | 生产优化函数必须具备五个纵切面证明：HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt；缺任一项时生产配置 no-op |
| 平台 | 每个启用架构独立通过 debug/release correctness、ASAN/refleak 和协议矩阵 |
| deopt/lifecycle | 覆盖 tp_clear、finalize、正常完成、显式 deopt、close/send 和 GC lifecycle，无泄漏或语义漂移 |
| 性能 | `generators` 接受标准通过；显式启用配置相对默认关闭配置有稳定收益，并在 21 个 JIT 用例列表上确认没有由本特性引入的显著整体回退 |
| 回滚 | 环境变量和 HIR pass 配置可完全关闭，关闭后 HIR 不含状态机指令 |

### 8.4.8 影响点列表

| 模块 | 影响 |
| ---- | ---- |
| `cinderx/Jit/hir/*` | 新增 pass、HIR 指令、类型/副作用/printer 支持 |
| `cinderx/Jit/compiler.*` | 接入 pass pipeline 和配置位 |
| `cinderx/Jit/config.*`、`pyjit.cpp` | 新增开关 |
| `cinderx/Jit/gen_data_footer.*` | 增加 TreeIter 状态指针，避免内嵌固定大栈 |
| `cinderx/Jit/jit_rt.cpp`、`generators_rt.cpp` | 初始化、显式状态分配、GC traverse、clear/dealloc 和实验 deopt gate 需要处理新增状态 |
| `cinderx/Jit/lir/*` | 新增 LIR opcode/lowering/postalloc 边界 |
| `cinderx/Jit/codegen/autogen.cpp` | 首版新增 AArch64 translate 规则；x86_64 首版只新增或复用 no-op arch gate，不实现 native translate |
| `cinderx/PythonLib/test_cinderx/*` | 新增 TreeIter 状态机测试 |
| benchmark 脚本 | 性能验收需增加 jitlist/auto 激活包装并固化量化 gate |

### 8.4.9 分配需求

| 需求 | 分配模块 |
| ---- | -------- |
| GJIT-SM-001 | pass-local TreeIter matcher |
| GJIT-SM-002 | generator protocol/deopt production gate 和协议回归矩阵 |
| GJIT-SM-003 | Heap-backed TreeIterState model |
| GJIT-SM-004 | matcher admission guards 和 bare None、non-iterable、子类覆盖等负例语义测试 |
| GJIT-SM-005 | Refcount and generator lifecycle |
| GJIT-SM-006 | Config, probe, tests |
| GJIT-SM-007 | Platform gate and architecture-specific codegen |
| GJIT-SM-008 | Production performance/protocol gate |
| GJIT-SM-009 | FieldAccessProof extractor、layout dependency、split-dict guard/deopt 和后续 OO-heavy 复用边界 |

状态机 HIR/LIR/codegen state ops 属于 SR-GJIT-004 的实现任务，不作为 GJIT-SM-004 的语义需求分配条目；GJIT-SM-004 的语义边界由准入 matcher 和负例测试共同承接。

# 9 详细设计与实现输入

详细设计和实现阶段必须重点闭合：

1. 当前 master HIR 中 guard、`GET_YIELD_FROM_ITER`、`Send`、`YieldValue::yieldFromIter()` 到 `self.left/right` 的精确追踪规则，包括 `is not None` guard、truthiness guard + default truthiness 证明、slot/member 字段和 split-dict fast path。
2. `GenDataFooter` 字段布局、初始化路径、释放路径和版本条件。
3. 每个新增 HIR 指令的 output type、memory effects、exception contract、replayable/passthrough 规则。
4. AArch64 codegen 的寄存器使用约束，以及 x86_64 首版 no-op gate 和后续验证矩阵。
5. postalloc move fold 对新原生 LIR 指令的屏障规则。
6. 首版实验路径和未来生产路径的 `gi_yieldfrom`、`send/throw/close`、StopIteration value 和异常传播边界。
7. 生产启用所需的 FrameState/reify 恢复模型；未实现前生产配置 no-op。
8. active-path/depth 的进入、离开、shared subtree、kExit marker 和引用所有权路径。
9. heap-backed growable stack 的扩容、失败和引用所有权路径。
10. pyperformance `generators` 直接性能 gate、非目标 generator 实验回归检查、21 个 JIT 用例生产/default rollout 回归检查、实验准入 artifact、生产五纵切面 artifact verifier 和完整协议回归 gate。

# 10 决策项状态

以下问题来自文档审查的 manual 类意见，均已按讨论确认。功能设计只定义生产准入原则和证据边界；具体采集方式、pass 阶段和数据结构由详细设计承接：

| 编号 | 状态 | 决策项 | 功能结论 |
| ---- | ---- | ------ | -------- |
| D-GJIT-SM-001 | 已决 | allowlisted 且已优化的 generator 上执行协议敏感操作时如何处理 | 生产路径采用精确 deopt/reify：`gi_yieldfrom`、`send(non-None)`、`throw`、`close` 发生前恢复为等价原始 generator/yield-from 状态，再走 CPython/CinderX 现有协议路径；M1.6 必须先证明 TreeIterState 到原始 frame/yield-from 栈可表示，无法精确 reify 的函数不得生产优化 |
| D-GJIT-SM-002 | 已决 | 有限无环树是否作为首版生产前提，以及生产环境如何处理环和深度 | 生产路径限定在有限无环树和原始递归 generator 可接受深度内；运行时维护 active-path 集合而不是全局 visited，进入 child 前发现回边或深度越界时精确 deopt/reify；详细设计必须给出 active-path/depth 转移不变量、shared subtree 退出语义和 kExit marker；无法 reify 时不触发生产状态机 |
| D-GJIT-SM-003 | 已决 | 生产准入证明来源的 artifact 格式 | 每个生产优化函数必须提交可验证的五个纵切面 artifact：HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt。每个纵切面必须列明证明来源和失效处理，并由当前 master artifact verifier fail-closed 校验；observed profile、本地样例或单次 HIR dump 只能作为候选信号，不能单独作为生产准入证据。任一纵切面缺少强证明或失效/deopt 机制时，生产配置必须 no-op |
| D-GJIT-SM-004 | 已决 | 是否为了 pyperformance `generators` 量身定制，或扩大到 21 个 JIT 用例 | 直接性能目标限定为 `generators` 所代表的递归对象树 `yield from` 模式，且 M4 必须以该用例达标作为 V2 成功条件；实现不得按 benchmark 名称硬编码，必须通过 `FieldAccessProof`、truthiness 等价性和 exactness/layout/deopt 证明来匹配。`FieldAccessProof` 作为可复用基础能力服务后续 OO-heavy 用例，但本轮不承诺 `richards`、`deltablue`、`go`、`hexiom`、`raytrace`、`chaos`、`float` 等非 generator 用例获得收益；替代 workload 只能触发后续 pivot 决策 |
