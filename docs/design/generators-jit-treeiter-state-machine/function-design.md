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

## 4 Keywords 关键词

CinderX, JIT, generator, yield from, TreeIter, HIR, LIR, GenDataFooter, 状态机, AArch64

## 5 Abstract 摘要

本文档定义基于当前 `master` 重新实现 generators JIT 优化的功能设计。旧分支 `github/bench-cur-7c361dce-claudecode` 中存在多份阶段性计划、研究、诊断和经验总结，内容覆盖 `OptimizedYieldFrom`、`InlineIter`、逃逸分析、状态机生成和 postalloc 调试。经整合后，本次重新实现只保留已证明有效的核心路径：针对语义可证明的树遍历生成器，在 JIT 编译期生成显式状态机，将递归生成器帧切换转换为 `GenDataFooter` 驱动的局部状态循环。

本文档不再把裸 `yield from self.left/right` 直接等同于空子树跳过。按 CPython 语义，`yield from None` 必须抛出 `TypeError`；状态机只有在原始源码/HIR 已显式包含空子树 guard，或编译期能证明子节点是同一精确 Node 类型且不会触发自定义 iterator 协议时，才允许替换原始 yield-from 路径。

本文档不直接移植旧分支的大量非 generators 改动、实验脚本、Docker 配置和阶段性文档；后续实现应以本文档作为功能边界，以当前 `master` 的 HIR 表示和 generator runtime 为准。V1.2 明确首版交付不是 production-ready 默认启用方案，而是默认关闭的实验核心：允许在受控 benchmark/test 配置中重建旧分支状态机核心逻辑，生产启用必须等 yield-from 协议、deopt reify、平台和验收矩阵全部闭合。

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

本功能域面向 CinderX JIT 对递归生成器的优化，首个实现目标只覆盖语义可证明的树遍历生成器模式。生产可优化形态必须把空子树跳过写入原始控制流，例如：

```python
def __iter__(self):
    if self.left is not None:
        yield from self.left
    yield self.value
    if self.right is not None:
        yield from self.right
```

未带 guard 的 `yield from self.left/right` 不是上述形态。若 `left/right` 为 `None`，CPython 会在 `GET_YIELD_FROM_ITER` / `PyObject_GetIter` 路径抛出 `TypeError`，状态机不得把它改写为空遍历。

该模式来自 pyperformance `generators` benchmark 和树结构遍历代码，但当前 master 是否能匹配正式 benchmark 不能只按源码形态推断。实现前必须用当前 master dump 目标 `Tree.__iter__` 的 HIR，证明 owner type、child type、iterator identity 和 guard 约束均满足；若证明失败，pyperformance 只能作为候选场景，不能作为首版覆盖承诺。原始 JIT 路径仍按 Python generator 语义维护递归生成器帧，每个 `yield from` 都可能触发子生成器创建、恢复入口查询、帧切换、`gi_yieldfrom` 暴露、`send/throw/close` 委派和状态保存。状态机优化将上述递归控制流转换为单个 JIT generator 内部的显式循环，但生产启用必须保留这些可观察语义，或在无法保留时拒绝优化：

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
| 树遍历生成器状态机 | 首版实验实现，默认关闭 |
| 泛化任意递归生成器 | 不涉及 |
| 消除所有 generator frame | 不涉及 |
| 逃逸分析驱动的 caller 内联 | 不作为首版依赖 |
| pyperformance 正式覆盖 | 需先给出当前 master HIR 准入证明；无证明时只做候选 benchmark |

生产状态：

| 项目 | 结论 |
| ---- | ---- |
| 首版交付定位 | 默认关闭的实验核心实现，用于复现旧分支 TreeIter 状态机收益和风险 |
| 实验启用条件 | 显式配置开启、目标架构已验证、目标 HIR 有准入证明、测试只声明 `next()`/`for` 消费语义 |
| 生产启用前置 | CPython yield-from 协议、deopt reify、release 栈安全、平台 codegen、GC/lifecycle 和验收矩阵全部闭合 |
| 不满足生产前置时 | 生产配置下 pass 必须 no-op；不得把实验状态机作为 production-ready 路径发布 |

## 8.2 功能域总体方案

总体方案分为五层：

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
状态机 HIR: Load/SaveCurrentNode, Load/SavePhase, StateStackPush/Pop, YieldValue
        |
        v
LIR + codegen: 通过 FP 正偏移读取 GenDataFooter.tree_iter_state 指针
        |
        v
运行时: GenDataFooter 分配、初始化、generator suspend/resume
```

设计原则：

1. 以当前 master 的 HIR 为输入，不要求先移植旧分支的 `YieldFrom`、`OptimizedYieldFrom`、`InlineIter` 全套指令。
2. `GenDataFooter` 只保存 TreeIter 状态指针，实际 current/phase/stack 放在按需分配的 heap-backed `TreeIterState` 中，避免给所有 JIT generator 增加固定 256B+ 状态数组。
3. 新增 HIR 指令按副作用保守建模，写操作和引用计数相关操作不得被 DCE/CSE 错误消除。
4. 首版只匹配原始空子树 guard、exact type/layout 和 iterator identity 都明确的 `left/right/value` 树遍历，任何不确定情况直接回退到原始 generator 路径。
5. codegen 首先保障 AArch64 路径；x86_64 可同步实现但必须单独验证后标记可用，未验证架构即使配置开启也 no-op。

## 8.3 功能域规格设计

### 8.3.1 功能规格

| 编号 | 规格 |
| ---- | ---- |
| GJIT-SM-001 | JIT 编译树遍历生成器时，只识别原始源码/HIR 已显式包含空子树 guard 的 `left/value/right` 中序遍历模式，或能证明子节点为同一精确 Node 类型且不会触发自定义 iterator 协议的模式 |
| GJIT-SM-002 | 生产可启用状态机必须保持 CPython 可观察 generator 语义与原始 generator 完全一致，包括异常、`gi_yieldfrom`、`send/throw/close`、StopIteration value 和 suspend/resume 状态；首版实验路径不声明满足该生产规格 |
| GJIT-SM-003 | 状态栈采用 heap-backed growable 结构；release 构建每次 push 前必须动态检查并安全扩容或报错，不能只依赖 debug 断言 |
| GJIT-SM-004 | 状态机路径不得把裸 `yield from None`、非 iterable、子类覆盖或自定义 iterator 协议改写为普通空遍历 |
| GJIT-SM-005 | 状态机相关 `PyObject*` 必须满足 CinderX RefcountInsertion、GC traverse、clear/dealloc、deopt 和 generator finalize 的所有权假设 |
| GJIT-SM-006 | 功能必须可通过独立 JIT 配置关闭，用于回归定位和性能对照 |
| GJIT-SM-007 | 目标架构未完成 codegen 和测试矩阵前，即使配置开启也必须 no-op |
| GJIT-SM-008 | 默认开启前必须具备当前 master HIR 准入证明、量化性能 gate 和完整协议回归 gate |

### 8.3.2 非目标

| 编号 | 非目标 |
| ---- | ------ |
| GJIT-SM-N001 | 不重写 CPython generator 协议；无法保持协议时拒绝优化 |
| GJIT-SM-N002 | 不在首版支持任意属性名、任意递归图、任意子类或用户自定义 iterator 协议内联 |
| GJIT-SM-N003 | 不把旧分支中与 ARM benchmark、Docker、wheel、其它 HIR 优化相关的改动纳入本功能 |
| GJIT-SM-N004 | 不承诺首版实验路径支持 deopt 后恢复到状态机精确位置；生产路径在精确 reify 未实现前必须 no-op |

## 8.4 功能项 TreeIter 状态机优化

### 8.4.1 功能概述

该功能项在 JIT HIR 优化阶段识别目标树遍历生成器，并用显式状态机替换原有递归 `yield from` 控制流。生成后的状态机通过 `GenDataFooter` 指针引用按需分配的 `TreeIterState` 保存：

| 状态字段 | 说明 |
| -------- | ---- |
| `current_node` | 当前遍历节点 |
| `current_phase` | 当前阶段：left、yield、right、backtrack |
| `stack_capacity` | 当前 heap stack 容量 |
| `stack_top` | 显式遍历栈栈顶 |
| `state_stack` | heap-backed growable 栈，保存待回溯的 `(node, phase)` |
| `popped_phase` | 单输出 HIR 限制下保存 `StateStackPop` 的 phase 结果 |

### 8.4.2 实现思路

原始逻辑必须已包含空子树 guard；若无 guard，则只有在编译期证明 child 非 None、同一 exact Node iterator 且不会触发自定义 iterator 协议时才可匹配：

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
  if original left guard says no child:
      phase = YIELD
  elif current.left is an exact supported child node:
      push(current, YIELD)
      current = current.left
      phase = LEFT
  else:
      fallback/deopt or reject optimization

YIELD:
  yield current.value
  phase = RIGHT

RIGHT:
  if original right guard says no child:
      phase = BACKTRACK
  elif current.right is an exact supported child node:
      current = current.right
      phase = LEFT
  else:
      fallback/deopt or reject optimization

BACKTRACK:
  if stack is empty:
      return None
  current, phase = pop()
```

该设计实质上把递归调用栈替换为显式栈，把生成器恢复点替换为 `current_phase`。显式栈首版使用初始容量 16 的 heap-backed growable 结构。release 路径必须在每次 push 前做容量检查；容量不足时调用 grow helper 扩容，扩容失败时走受控错误路径，不能越界写或只依赖 debug 断言。

### 8.4.3 实现设计

#### 8.4.3.1 模式识别设计

准入条件：

| 条件 | 要求 |
| ---- | ---- |
| 代码对象 | generator function，且字段名集合包含 `left`、`right`、`value` |
| HIR 形态 | 当前 master 中应从 `YieldValue::isYieldFrom()` 或对应 `Send` 链路找到 yield-from iterator，并完整识别 `GET_YIELD_FROM_ITER` 生成的 coroutine rejection、exact generator assign、slow-path `GetIter` 和 merge 结构 |
| 空子树语义 | 必须在原始源码/HIR 中存在显式 `is not None` 等价 guard；不得给裸 `yield from self.left/right` 人为增加 `None` 为空的语义 |
| 字段来源 | yield-from iterator 来源必须能追溯到 `self.left` 或 `self.right` 字段加载，并能证明非空 child 是同一精确 Node 类型或静态 sealed 类型 |
| iterator identity | child 的 `__iter__` 必须解析到当前被优化的 code object；若存在子类覆盖、descriptor/property 副作用或自定义 iterator 协议，必须回退 |
| 产出值 | 普通 `yield` 值必须能追溯到同一精确节点布局的 `value` 字段 |
| 控制流 | 仅允许匹配目标树遍历骨架；出现额外副作用、不明调用、异常处理复杂路径或无法精确 deopt 的节点时回退 |

检测失败必须保持原 HIR 不变。

#### 8.4.3.2 状态存储设计

状态机不在 HIR SSA 中创建循环 Phi。原因是旧分支证明 Phi 自引用与后续 CopyPropagation、类型重推导、寄存器分配组合风险较高。`GenDataFooter` 只新增一个稳定 offset 的 `TreeIterState*`，实际状态放在按需分配的 heap 对象中。

逻辑数据结构：

```text
GenDataFooter
  tree_iter_state: TreeIterState*

TreeIterState
  current_node: PyObject*
  current_phase: int32
  stack_top: int32
  stack_capacity: int32
  popped_phase: int32
  state_stack:
    node: PyObject*
    phase: int32
```

容量策略：

| 项目 | 首版规格 |
| ---- | -------- |
| 初始栈条目数 | 16 |
| 单条目大小 | 16 bytes |
| 支持深度 | 目标覆盖 depth <= 12 的树遍历验证场景 |
| 超限行为 | release 构建每次 push 前动态检查；容量不足时扩容 heap stack，扩容失败时抛出受控错误并保持引用所有权一致 |

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

1. `SaveCurrentNode(self)` 和 `SavePhase(LEFT)` 必须插入到 `InitialYield` 之前，避免 `InitialYield` 后 caller-saved register 被覆盖。
2. 新插入指令必须满足定义先于使用，不能用追加到块末尾的辅助函数在 `InitialYield` 前创建依赖。
3. `YieldValue` 必须携带原始 `FrameState`；如果无法安全构造 FrameState，则回退。
4. 状态机生成后必须重新清理不可达块，并对新寄存器做类型重推导，避免 `TTop` 被误当作 object 导致错误 decref。

#### 8.4.3.4 HIR/LIR/codegen 支持设计

新增状态机专用逻辑指令：

| 指令 | 类型 | 语义 | 副作用策略 |
| ---- | ---- | ---- | ---------- |
| `SaveCurrentNode(node)` | HIR | 更新 `TreeIterState.current_node`，必要时 lazy 分配状态对象 | 写状态并处理 refcount，必须保守 |
| `LoadCurrentNode()` | HIR | 读取当前节点 | 返回 object，需要给寄存器独立引用，必须保守 |
| `SavePhase(phase)` | HIR | 更新 `TreeIterState.current_phase` | 写状态，必须保守 |
| `LoadPhase()` | HIR | 读取 `current_phase` | 纯读，可在确认安全后放宽 |
| `StateStackPush(node, phase)` | HIR | 入栈并持有 node 引用，容量不足时扩容 heap stack | 写状态并处理 refcount，必须保守 |
| `StateStackPop()` | HIR | 出栈 node，phase 写到 `popped_phase` | 写状态并转移引用，必须保守 |
| `LoadPoppedPhase()` | HIR | 读取最近 pop 的 phase | 纯读，可在确认安全后放宽 |
| `LoadStackTop()` | HIR | 读取栈顶 | 纯读，可在确认安全后放宽 |

生产协议约束：

| 项目 | 约束 |
| ---- | ---- |
| `gi_yieldfrom` | 如果优化后 generator 在原语义中应处于 delegated yield-from 状态，必须保留可观察元数据，或在可观察前精确 deopt |
| `send(non-None)` | 必须按 CPython yield-from 语义委派到当前子 iterator；无法委派时不得优化 |
| `throw/close/finalize` | 必须能传播到当前 delegated iterator 或精确 deopt 到等价解释器状态 |
| StopIteration value | 必须保留 yield-from 完成值处理；目标 TreeIter 不使用该值也不能改变异常传播 |

codegen 分层：

| 阶段 | 首版要求 |
| ---- | -------- |
| HIR | 定义 op、类型、printer、parser 需要的最小支持 |
| instr effects | 写操作和 refcount 操作默认 `hasArbitraryExecution=true`；纯读操作可在测试覆盖后优化 |
| LIR lowering | 状态机热路径使用原生 LIR opcode，不使用 `appendCallInstruction` 伪装成 C 调用 |
| AArch64 codegen | 通过 frame pointer 正偏移读取 `GenDataFooter.tree_iter_state`，再访问 heap state 字段 |
| x86_64 codegen | 可与 AArch64 同步实现，但发布前必须单独验证 |

#### 8.4.3.5 引用计数设计

所有状态机中保存的 `PyObject*` 必须明确所有权：

| 操作 | 引用计数规则 |
| ---- | ------------ |
| 保存 current node | 对新节点 `INCREF`，对旧节点 `DECREF` |
| 读取 current node | 返回前 `INCREF`，交给 RefcountInsertion 后续 `XDecref` |
| stack push | 栈持有节点引用，push 时 `INCREF` |
| stack pop | 栈引用转移给输出寄存器，pop 后清空槽位 |
| generator data 释放 | 清理 `TreeIterState.current_node`、栈内残留引用和 heap stack |

该规则来自旧分支 depth>=3 崩溃的根因：运行时函数返回 borrowed reference 后被 RefcountInsertion 自动 `XDecref`，导致树节点被提前释放。

#### 8.4.3.6 配置与观测设计

| 项目 | 设计 |
| ---- | ---- |
| 配置项 | 新增独立 HIR optimization 配置，例如 `tree_iter_state_machine` |
| 环境变量 | 提供 `PYTHONJITTREEITERSTATEMACHINE` 开关 |
| 默认值 | 首版默认关闭，仅在实验配置中开启；通过生产 gate 后才允许评估默认开启 |
| 触发探针 | 提供 debug/test-only counter，用于确认 pass 是否触发 |
| 日志 | 使用 JIT debug 日志，不向 stdout/stderr 输出热路径日志 |

### 8.4.4 增量SR清单

| 里程碑 | SR编号 | 需求描述 | 是否允许生成状态机 HIR |
| ------ | ------ | -------- | ------------------------ |
| M0 | SR-GJIT-001 | 新增配置、触发探针和 no-op pass 框架，默认关闭 | 否 |
| M1 | SR-GJIT-002 | 模式识别、当前 master HIR 准入证明和负例覆盖，失败时无行为变化 | 否 |
| M2 | SR-GJIT-003 | heap-backed `TreeIterState`、引用清理、GC traverse、release 扩容安全路径 | 否 |
| M3 | SR-GJIT-004 | 在显式实验开关下生成 TreeIter 状态机 CFG、HIR/LIR/codegen 和 `next()`/`for` correctness 测试 | 仅实验配置 |
| M4 | SR-GJIT-005 | AArch64 实验性能 gate 和非目标 generator 回退检查 | 仅实验配置 |
| M5 | SR-GJIT-006 | 精确 yield-from/deopt 协议、lifecycle、平台矩阵和生产性能 gate 全部闭合 | 是，生产配置 |
| 后续演进 | SR-GJIT-F001 | 泛化到更多递归生成器模式 | 否，不纳入首版 SR |

### 8.4.5 实现接口设计

#### 8.4.5.1 实现接口设计

模块间逻辑接口：

```text
Compiler pass pipeline
  -> TreeIterStateMachinePass.run(function)
     -> pass-local matcher(function)
     -> pass-local state-machine builder(function, field layout, frame state)
     -> TreeIterState pointer ops
     -> LIR/codegen lowering
```

接口约束：

1. Pass 输入输出均为 HIR `Function`，不得依赖 Python 层测试脚本。
2. matcher 是 pass-local helper，不作为可复用公共抽象；它不修改 HIR。
3. builder 是 pass-local helper，只有完整匹配后才允许改写 CFG；生成失败必须可放弃改写，不能留下半状态机 CFG。
4. `GenDataFooter` 只暴露稳定的 `TreeIterState*` offset，状态对象分配和清理路径统一初始化。
5. LIR/codegen 不得 clobber allocator 可分配寄存器中的活跃值；新增原生指令必须纳入 postalloc 优化边界测试。

#### 8.4.5.2 实现接口定义

| 接口 | 输入 | 输出 | 说明 |
| ---- | ---- | ---- | ---- |
| `TreeIterStateMachinePass` | HIR function | HIR function 或不变 | 编译期模式识别与 CFG 改写入口 |
| pass-local matcher | HIR function | 匹配结果：字段偏移、yield frame state、self register | 只服务 TreeIterStateMachinePass，不引入公共 PatternDetector 抽象 |
| pass-local builder | 匹配结果 | 新状态机基本块集合 | 只负责本 pass 的 CFG 构造和原始路径替换 |
| TreeIter state HIR ops | node/phase/stack 操作 | 状态读写 HIR | 访问 `GenDataFooter.tree_iter_state` 指向的 heap state |
| LIR translate rules | 状态机 LIR op | 目标架构机器码 | 接入现有 AArch64/x86_64 codegen，不创建独立 StateMachineCodegen 框架 |
| `JIT config` | 环境变量或配置对象 | pass enable/disable | 控制功能开关 |

### 8.4.6 功能规格设计

#### 8.4.6.1 正常流程

```text
1. HIR builder 构建当前 master 的 generator HIR。
2. 常规 simplify / cleanup 后进入 TreeIterStateMachinePass。
3. pass 确认 function 是带原始空子树 guard、exact type/layout 和 iterator identity 的目标树遍历 generator。
4. pass 提取 left/right/value 字段偏移、self register、原始 guard、YieldValue FrameState 和实验/生产 gate 能力。
5. pass 在 InitialYield 前保存初始 current node 和 phase。
6. pass split 原入口块并生成状态机 CFG。
7. 状态机执行中通过 `GenDataFooter.tree_iter_state` 指向的 heap state 保存 current node、phase 和显式栈。
8. 每次 YIELD 阶段读取 current.value；首版实验只声明受控 `next()`/`for` 消费结果正确，生产路径在无法保留协议语义时不触发优化。
9. 栈为空且 phase 为 backtrack 时清理 tree_iter 状态并返回 None，generator 正常结束。
```

#### 8.4.6.2 异常与回退流程

| 场景 | 行为 |
| ---- | ---- |
| 字段名或 HIR 链路不匹配 | 不改写，走原始 generator |
| 缺少可复用 FrameState | 不改写，走原始 generator |
| 状态栈容量不足 | release 必须在写入前动态检查并扩容 heap stack；扩容失败走受控错误路径 |
| `yield from None` 或非 iterable | 保留 CPython 异常语义，不得作为空子树跳过 |
| `send/throw/close/gi_yieldfrom` 无法保持 | 生产配置不改写，走原始 generator；首版实验配置只覆盖已声明的 `next()`/`for` 受控场景 |
| refcount 或 dealloc 罕见路径 | codegen 需调用安全 dealloc 或退回 C helper |
| postalloc 优化破坏 move 链 | 新指令必须成为优化边界或补充中间寄存器使用检查 |
| 目标架构未验证 | 配置关闭或仅启用解释/原始 JIT 路径 |

#### 8.4.6.3 验收规格

| 类型 | 规格 |
| ---- | ---- |
| 正确性 | depth 1-12 完整中序遍历结果与 Python 原始实现一致，且裸 `yield from None`、非 iterable、显式 guard 三类语义分别正确 |
| 稳定性 | 同一棵树重复遍历，多次 generator 创建和销毁无崩溃、无悬挂引用 |
| 准入 | 非 TreeIter generator 不触发 pass |
| 可控性 | 环境变量关闭时 HIR 不含状态机专用指令 |
| 协议 | 生产启用前 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、StopIteration value、异常上下文必须与 CPython 原语义一致；首版实验只覆盖受控 `next()`/`for` 消费 |
| 溢出 | depth 16、17、极深 skewed tree 和循环对象图在 release 下不越界、不崩溃、不产生错误结果 |
| 性能 | 指定平台和命令下，相对当前 master + feature off 达到量化收益阈值，非目标 generator 无统计显著回退 |
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

##### 8.4.7.1.1 FMEA分析

| 失效模式 | 影响 | 原因 | 检测方式 | 预防措施 |
| -------- | ---- | ---- | -------- | -------- |
| 提前释放节点 | SIGSEGV 或错误遍历 | 返回 borrowed ref 后被 XDecref | depth>=3、多次遍历、ASAN | Load 返回前 INCREF，Save/Push 持有引用 |
| 状态机只遍历左叶子 | 结果错误 | SavePhase 被优化器消除或 LoadPhase 被 CSE | depth 1-12 正确性测试 | 写操作保守副作用 |
| Linux 偶发崩溃 | 非确定性 SIGSEGV | free-list 复用未清零状态字段 | 多轮 kunpeng 稳定性测试 | 分配路径显式初始化 |
| 编译后寄存器读垃圾 | 崩溃或错误节点 | InitialYield clobber self register | HIR dump 检查 init 顺序 | SaveCurrentNode 位于 InitialYield 前 |
| postalloc 删除返回值 move | 错误 decref 或 stale register | fold 未检查中间使用 | LIR dump 对比 | 增加优化屏障和中间使用检查 |
| `yield from None` 被跳过 | 本应抛出的 TypeError 消失 | 把 `None` 当空子树而非原始 guard | None/非 iterable 语义测试 | 只匹配原始 guard，裸 yield-from 保留异常 |
| close/throw 未委派 | generator finalization 或异常传播错误 | 普通 YieldValue 丢失 delegated iterator 元数据 | 生产 gate 协议测试 | 首版实验不声明支持；生产启用前必须保留 yield-from 元数据或精确 deopt |

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

默认开启前至少满足：

| Gate | 要求 |
| ---- | ---- |
| 平台 | 每个启用架构独立通过 debug/release correctness、ASAN/refleak；生产启用还需协议矩阵 |
| deopt/lifecycle | 实验路径覆盖 tp_clear、finalize、正常完成无泄漏；生产启用还需 instrumentation attach、close、throw 无语义漂移 |
| 性能 | 目标 benchmark 相对 feature off 达到预设阈值，且非目标 generator 无显著回退 |
| 回滚 | 环境变量和 HIR pass 配置可完全关闭，关闭后 HIR 不含状态机指令 |

### 8.4.8 影响点列表

| 模块 | 影响 |
| ---- | ---- |
| `cinderx/Jit/hir/*` | 新增 pass、HIR 指令、类型/副作用/printer 支持 |
| `cinderx/Jit/compiler.*` | 接入 pass pipeline 和配置位 |
| `cinderx/Jit/config.*`、`pyjit.cpp` | 新增开关 |
| `cinderx/Jit/gen_data_footer.*` | 增加 TreeIter 状态指针，避免内嵌固定大栈 |
| `cinderx/Jit/jit_rt.cpp`、`generators_rt.cpp` | 初始化、lazy 分配、GC traverse、clear/dealloc 和实验 deopt gate 需要处理新增状态 |
| `cinderx/Jit/lir/*` | 新增 LIR opcode/lowering/postalloc 边界 |
| `cinderx/Jit/codegen/autogen.cpp` | 新增 AArch64/x86_64 translate 规则 |
| `cinderx/PythonLib/test_cinderx/*` | 新增 TreeIter 状态机测试 |
| benchmark 脚本 | 性能验收需增加 jitlist/auto 激活包装并固化量化 gate |

### 8.4.9 分配需求

| 需求 | 分配模块 |
| ---- | -------- |
| GJIT-SM-001 | pass-local TreeIter matcher |
| GJIT-SM-002 | pass-local builder + HIR CFG |
| GJIT-SM-003 | Heap-backed TreeIterState model |
| GJIT-SM-004 | HIR/LIR/codegen state ops |
| GJIT-SM-005 | Refcount and generator lifecycle |
| GJIT-SM-006 | Config, probe, tests |
| GJIT-SM-007 | Platform gate and architecture-specific codegen |
| GJIT-SM-008 | Production performance/protocol gate |

# 9 详细设计与实现输入

详细设计和实现阶段必须重点闭合：

1. 当前 master HIR 中 guard、`GET_YIELD_FROM_ITER`、`Send`、`YieldValue::yieldFromIter()` 到 `self.left/right` 的精确追踪规则。
2. `GenDataFooter` 字段布局、初始化路径、释放路径和版本条件。
3. 每个新增 HIR 指令的 output type、memory effects、replayable/passthrough 规则。
4. AArch64 codegen 的寄存器使用约束，以及 x86_64 验证矩阵。
5. postalloc move fold 对新原生 LIR 指令的屏障规则。
6. 首版实验路径和未来生产路径的 `gi_yieldfrom`、`send/throw/close`、StopIteration value 和异常传播边界。
7. 生产启用所需的 FrameState/reify 恢复模型；未实现前生产配置 no-op。
8. heap-backed growable stack 的扩容、失败和引用所有权路径。
9. pyperformance HIR 准入证明、量化性能 gate 和完整协议回归 gate。
