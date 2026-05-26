# 详细设计说明书：Generators JIT TreeIter 状态机优化

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
| V1.0 | 2026-05-26 | Codex Agent | 基于当前 master、功能设计文档和 `github/bench-cur-7c361dce-claudecode` 原实现重写详细设计 |
| V1.1 | 2026-05-26 | Codex Agent | 按审校意见补充 CPython yield-from 协议、准入、deopt/lifecycle、release 栈溢出和生产 gate |
| V1.2 | 2026-05-26 | Codex Agent | 按二轮审校意见固定首版实验交付、heap-backed 状态栈、生产 no-op gate 和内存影响边界 |

## 4 Keywords 关键词

CinderX, JIT, generator, yield from, TreeIter, HIR, LIR, GenDataFooter, state machine, refcount, AArch64, postalloc

## 5 Abstract 摘要

本文档描述基于当前 `master` 重新实现 generators JIT TreeIter 状态机优化的详细设计。输入来自上一阶段功能设计文档 `docs/design/generators-jit-treeiter-state-machine/function-design.md`，实现证据来自旧分支 `github/bench-cur-7c361dce-claudecode` 中已完成的 `TreeIterStateMachinePass`、`GenDataFooter` 扩展、状态机 HIR/LIR 指令、AArch64/x86_64 codegen 和调试经验。旧分支仅作为 MVP 参考；正确性以当前 CinderX 源码和 `/opt/Claude-Code/cpython` 源码为准。

本次详细设计不按旧分支逐文件复制。旧分支的核心算法保留，但接口适配当前 `master`：当前 HIR 使用 `Send` 与 `YieldValue::yieldFromIter()` 表示 yield-from，不再引入旧分支的显式 `YieldFrom`、`OptimizedYieldFrom`、`InlineIter` 作为必要前置。V1.2 将首版收敛为默认关闭的实验核心实现：选择 heap-backed growable `TreeIterState` 解决 release 栈溢出和全局内存膨胀问题；不把 `gi_yieldfrom` / `send` / `throw` / `close` / suspended deopt 精确协议声明为首版生产能力。生产配置在精确 reify 和协议模型未实现前必须 no-op。

## 6 List of abbreviations 缩略语清单

| 缩略语 | 英文全称 | 中文名 |
| ------ | -------- | ------ |
| CFG | Control Flow Graph | 控制流图 |
| DCE | Dead Code Elimination | 死代码消除 |
| GC | Garbage Collection | 垃圾回收 |
| HIR | High-level Intermediate Representation | 高层中间表示 |
| LIR | Low-level Intermediate Representation | 低层中间表示 |
| JIT | Just-In-Time Compilation | 即时编译 |
| SSA | Static Single Assignment | 静态单赋值形式 |

## 7 简介

### 7.1 背景

树遍历生成器通常写作：

```python
def __iter__(self):
    if self.left is not None:
        yield from self.left
    yield self.value
    if self.right is not None:
        yield from self.right
```

普通 JIT generator 路径会保留 Python generator 的递归帧切换和 yield-from 恢复机制。本文档的状态机只允许在原始源码/HIR 已经包含空子树 guard，或编译期能证明 child 是同一精确节点类型且不会触发自定义 iterator 协议时启用。裸 `yield from self.left/right` 不具备 `None` 为空的语义；按 CPython `GET_YIELD_FROM_ITER`，`yield from None` 必须抛 `TypeError`。

旧分支验证过，单独优化 `yield from` 运行时调用或逃逸分析收益有限，真正有效的路径是把上述递归结构转换为显式状态机：

```text
current_node + current_phase + state_stack
    -> LEFT
    -> YIELD
    -> RIGHT
    -> BACKTRACK
```

### 7.2 本文档目标

本文档作为实现前的详细设计输入，回答以下问题：

1. 当前 `master` 中如何识别带空子树 guard 和精确类型约束的 TreeIter yield-from HIR。
2. 新增哪些内部数据结构、HIR 指令、LIR 指令和 codegen 规则。
3. 状态机 CFG 如何在显式实验配置下生成，以及哪些语义不允许声明为生产能力。
4. `PyObject*` 引用如何在 `GenDataFooter`、heap-backed `TreeIterState`、寄存器和显式栈之间转移。
5. 首版如何闭合 release 栈溢出、GC/finalize/tp_clear；生产 deopt、`gi_yieldfrom`、`send/throw/close` 仍由生产 gate 阻止。
6. 需要在哪些现有文件中接入配置、初始化、GC、deopt、测试和性能验证。

# 8 上游文档引用

| 类型 | 路径或来源 | 用途 |
| ---- | ---------- | ---- |
| 功能设计 | `docs/design/generators-jit-treeiter-state-machine/function-design.md` | 功能边界、DFX、验收规格 |
| 当前 master HIR builder | `cinderx/Jit/hir/builder.cpp` | `YieldValue::setYieldFromIter()` 生成逻辑 |
| 当前 master HIR 定义 | `cinderx/Jit/hir/hir.h` | `YieldValue`、`Send`、`InitialYield` 当前接口 |
| 当前 master pass pipeline | `cinderx/Jit/compiler.cpp`、`cinderx/Jit/compiler.h` | pass 插入点和配置位 |
| 当前 master generator data | `cinderx/Jit/gen_data_footer.h`、`cinderx/Jit/jit_rt.cpp`、`cinderx/Jit/generators_rt.cpp` | `GenDataFooter` 布局、分配、GC、deopt |
| 原分支状态机 pass | `github/bench-cur-7c361dce-claudecode:cinderx/Jit/hir/tree_iter_state_machine_pass.*` | 模式识别、CFG 生成和状态机阶段 |
| 原分支状态字段 | `github/bench-cur-7c361dce-claudecode:cinderx/Jit/gen_data_footer.h` | `current_node/current_phase/state_stack` 设计 |
| 原分支 HIR/LIR/codegen | `github/bench-cur-7c361dce-claudecode:cinderx/Jit/hir/hir.h`、`lir/generator.cpp`、`codegen/autogen.cpp` | 新指令、lowering 和直接 FP offset codegen |
| 原分支经验文档 | `docs/superpowers/generators/2026-03-30-tree-iter-state-machine-lessons-learned.md`、`2026-04-01-regalloc-investigation-report.md` | refcount、SSA、postalloc 等风险 |

# 9 实现设计 TreeIter 状态机重实现

## 9.1 实现概述

### 9.1.1 文件级改动

| 文件 | 改动 |
| ---- | ---- |
| `cinderx/Jit/hir/tree_iter_state_machine_pass.h` | 新增 pass、匹配结果、状态机生成器声明 |
| `cinderx/Jit/hir/tree_iter_state_machine_pass.cpp` | 新增模式识别、CFG 生成、回退逻辑 |
| `cinderx/Jit/hir/hir_ops.h` | 新增状态机 HIR opcode |
| `cinderx/Jit/hir/hir.h` | 定义状态机 HIR 指令类 |
| `cinderx/Jit/hir/instr_effects.cpp` | 定义内存效果、任意执行属性 |
| `cinderx/Jit/hir/pass.cpp` | 定义输出类型和必要的 passthrough/replayable 行为 |
| `cinderx/Jit/hir/printer.cpp` | 支持 HIR dump 输出 |
| `cinderx/Jit/hir/parser.cpp` | 如测试需要 round-trip HIR，补充 parser 支持 |
| `cinderx/Jit/compiler.h`、`compiler.cpp` | 新增 pass config bit 并接入 pipeline |
| `cinderx/Jit/config.h`、`pyjit.cpp` | 新增 `tree_iter_state_machine` 配置和环境变量 |
| `cinderx/Jit/gen_data_footer.h` | 增加 `TreeIterState*` 指针，不内嵌固定栈数组 |
| `cinderx/Jit/jit_rt.cpp` | 初始化 footer 指针为空，实验路径按需分配状态对象 |
| `cinderx/Jit/generators_rt.cpp` | GC traverse、clear/dealloc、实验 deopt gate 处理 TreeIter 状态 |
| `cinderx/Jit/lir/instruction.h` | 新增状态机 LIR opcode |
| `cinderx/Jit/lir/generator.cpp` | HIR 状态机指令 lowering 到原生 LIR |
| `cinderx/Jit/codegen/autogen.cpp` | 新增 AArch64/x86_64 translate 函数和规则 |
| `cinderx/PythonLib/test_cinderx/test_jit_generators.py` | 新增 TreeIter correctness、触发、回退测试 |

构建系统当前通过 `CMakeLists.txt` 中 `file(GLOB_RECURSE JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/*.cpp ${PROJECT_SOURCE_DIR}/Jit/*.c)` 收集 JIT 源文件，因此新增 `Jit/hir/tree_iter_state_machine_pass.cpp` 不需要手工加入源列表。

### 9.1.2 实现层次

```text
当前 master HIR
  guard(left/right is not None), YieldValue(isYieldFrom=true), Send,
  GET_YIELD_FROM_ITER CFG, LoadField
        |
        v
pass-local TreeIter matcher
  TreeIterMatch{self, exact node type, left/right/value offsets,
                original guard blocks, frame state, initial yield}
        |
        v
pass-local state-machine builder
  生成 Load/SaveCurrentNode、Load/SavePhase、StateStackPush/Pop、YieldValue
        |
        v
LIR lowering
  生成 kLoadPhase/kSavePhase/kLoadCurrentNode/kSaveCurrentNode/...
        |
        v
Codegen
  通过 FP + offsetof(GenDataFooter, tree_iter_state) 读取状态指针，
  再访问 heap-backed TreeIterState 字段
```

### 9.1.3 首版交付闭环和生产边界

首版不是 production-ready 默认启用方案，而是显式实验交付。二轮审校后固定如下闭环，避免实现阶段继续在互斥模型之间摇摆：

| 主题 | V1.2 决策 | 结果 |
| ---- | --------- | ---- |
| 交付定位 | 默认关闭的实验核心，面向 `next()` / `for` / `list()` 受控消费 | 不声明完整 generator 协议生产完备 |
| release 栈溢出 | 选择 heap-backed growable `TreeIterState`，初始容量 16，push 前检查并扩容 | 不再使用固定 16 项 footer 内嵌栈作为 release 边界 |
| footer 内存影响 | `GenDataFooter` 只新增一个 `TreeIterState*` 指针 | 非 TreeIter JIT generator 不承担 256B+ 固定栈开销 |
| yield-from 协议 | 首版实验不实现 `gi_yieldfrom` / `send(non-None)` / `throw` / `close` 等价模型 | 生产配置在协议模型未实现前 no-op |
| suspended deopt | 首版实验不实现 footer state 到解释器 yield-from 栈的精确 reify | instrumentation、`close()`、`throw()` 的生产路径必须禁用状态机 |
| pyperformance 覆盖 | 必须先 dump 当前 master HIR，证明 owner/child/iterator identity 满足准入 | 无证明时只能作为候选 benchmark，不作为覆盖承诺 |

因此本文后续的状态机 CFG、HIR/LIR/codegen 描述属于实验路径。任何生产启用必须新增精确 deopt/protocol 设计并通过 9.6.6 和测试矩阵。

## 9.2 关键算法与流程

### 9.2.1 TreeIter 模式识别算法

当前 `master` 中 yield-from 的关键证据：

1. `HIRBuilder::emitYieldValue()` 在 Python 3.14+ 中，当 `YIELD_VALUE` oparg 为 1 时创建 `YieldValue` 并调用 `setYieldFromIter(stack.top())`。
2. `YieldValue` 内部有 `yieldFromIter_` 字段，`isYieldFrom()` 为 true 时表示该 yield 是 yield-from 中间值。
3. `Send` 指令保存 iterator 和 send value，但本设计不依赖引入旧分支的显式 `YieldFrom` 指令。
4. `HIRBuilder::emitGetYieldFromIter()` 会生成 coroutine rejection、exact generator/coroutine 直接 `Assign`、slow-path `GetIter` 和 done-block 合流结构；matcher 不能只搜索单一 `GetIter` 节点。

匹配流程：

```text
matchTreeIter(function):
  1. 校验 function.code 不为空，co_names 至少包含 left/right/value。
  2. 校验函数可解析为 owner type method，owner type 必须是 heap type、generic getattr、无子类或等价 sealed/static 类型。
  3. 遍历 CFG，收集：
     - InitialYield
     - LoadArg(0) 对应 self_reg
     - 普通 YieldValue，即 !isYieldFrom()
     - yield-from YieldValue，即 isYieldFrom()
     - left/right 的原始 None guard blocks
  4. 对每个 yield-from YieldValue：
     - iter_reg = yv.yieldFromIter()
     - 追踪 iter_reg 的 producer，兼容 emitGetYieldFromIter 的 Assign/GetIter/Phi 合流
     - 保留或证明不需要 coroutine rejection 和 GetIter TypeError 语义
     - 最终 iterable 来源必须是 LoadField(base=current/self, name in {left, right})
  5. 验证每个 left/right yield-from 前存在原始 None guard；如果没有 guard，则 child 必须被证明为非 None 且同一 exact Node iterator。
  6. 对普通 YieldValue：
     - value_reg = yv.reg()
     - 最终必须追溯到 LoadField(name=value)
     - value 字段必须来自同一 exact Node 布局
  7. 检查 yield-from 字段顺序为 left, right，普通 yield 位于二者之间。
  8. 提取字段 offset、原 YieldValue FrameState、InitialYield block、实验/生产 gate 能力标志。
```

`traceYieldFromIterable(iter_reg)` 需要兼容当前 master 和旧分支已验证的形态：

```text
iter_reg.def == GetIter
iter_reg.def == Phi(..., GetIter, ...)
iter_reg.def == Phi(..., Assign(original_iterable), GetIter(original_iterable), ...)
iter_reg.def == Assign(original_iterable)
```

若 trace 结果需要删除 coroutine TypeError 或 non-iterable TypeError，则返回 no match，不修改 CFG。若生产配置无法保留 `send/throw/close` 委派或 `gi_yieldfrom` 元数据，也必须 no-op；首版实验配置仅允许在已声明的 `next()`/`for` 受控范围内生成状态机。

### 9.2.2 字段追踪规则

字段追踪使用保守 unwrap：

```text
unwrapFieldSource(reg):
  instr = reg->instr()
  while instr is one of allowed transparent checks:
      instr = instr.primary_object_operand()->instr()
  if instr is LoadField:
      return {base_reg, name, offset, type}
  return none
```

首版允许的透明检查只包含当前代码中明确可证明不改变对象身份的节点，例如 `CheckField`。不得把可能执行 Python 代码或读取用户可变状态的节点加入白名单。

字段追踪成功后还必须满足类型和 iterator identity：

| 条件 | 要求 |
| ---- | ---- |
| owner type | `self` 对应 type 必须可解析为当前 code object 的 owner method，且无子类或等价 sealed/static 约束 |
| child type | 非空 `left/right` 必须 guard 为同一 exact owner type，或字段类型系统能证明不会是任意 iterable |
| iterator identity | child 的 `__iter__` 必须解析到同一 code object；若可能调用用户自定义 `__iter__`，回退 |
| field layout | `left/right/value` offset、descriptor 和 type version guard 必须覆盖布局变化 |
| sentinel | 空子树 sentinel 只能来自原始 guard；不得在状态机中新增 `None` 为空的语义 |

实现前必须提供至少一个当前 master HIR 准入样例：

| 样例 | 要求 |
| ---- | ---- |
| pyperformance `generators` | dump `Tree.__iter__` HIR，证明 owner type 无子类或等价 sealed、child exact type、iterator identity 和 guard 均能从当前 HIR/Preloader 推导 |
| 本地回归 TreeIter | 若 pyperformance 不满足准入，提供测试内 sealed/static 或其它可证明形态，且性能口径不得宣称覆盖 pyperformance |
| 负例 | 子类覆盖、descriptor/property、副作用 `__iter__`、裸 `yield from None` 均不触发状态机 |

### 9.2.3 状态机 CFG 生成算法

状态机阶段：

```cpp
enum class TreeIterPhase : int32_t {
  kLeft = 0,
  kYield = 1,
  kRight = 2,
  kBacktrack = 3,
};
```

生成块：

```text
bb_init
bb_loop
bb_check_yield
bb_check_right
bb_left
bb_check_null_left
bb_has_left
bb_no_left
bb_yield
bb_after_yield
bb_right
bb_check_null_right
bb_has_right
bb_no_right
bb_backtrack
bb_pop
bb_done
```

关键生成步骤：

```text
1. 找到 InitialYield 所在 init_block。
2. 在 InitialYield 前插入：
   SaveCurrentNode(self_reg)
   LoadConst(kLeft)
   SavePhase(kLeft)
3. splitAfter(InitialYield)，保留 init_block 作为状态机入口前置块。
4. AllocateBlock 创建状态机块。
5. init_block append Branch(bb_loop)。
6. bb_loop LoadPhase 后做 kLeft/kYield/kRight/kBacktrack dispatch。
7. left/right 阶段只重放原始 guard 已证明的空子树分支；不得新增裸 `Py_None` 为空的语义。
8. 对非空 child 执行 exact type/layout guard；失败时走安全慢路径或拒绝优化。
9. yield 阶段 LoadCurrentNode、LoadField(value)、YieldValue，并按协议设计保留或精确 deopt `yield from` 可观察状态。
10. backtrack 阶段 LoadStackTop，空栈到 done，非空到 pop。
11. pop 阶段 StateStackPop、LoadPoppedPhase、SaveCurrentNode、SavePhase。
12. done 阶段先清理 tree_iter 状态引用，再 Return Py_None。
13. 删除不可达块，重新推导新寄存器类型。
```

初始化插入必须使用 `BasicBlock::insert()`，不得调用会 append 到块末尾的辅助函数创建被 `SavePhase` 使用的常量。否则会出现定义晚于使用的 SSA 违规。

### 9.2.4 状态转移算法

```text
LEFT:
  current = LoadCurrentNode()
  left = current.left
  if original left guard proves no child:
      SavePhase(kYield)
  else if left is not an exact supported Node:
      fallback/deopt or reject optimization
  else:
      StateStackPush(current, kYield)
      SaveCurrentNode(left)
      SavePhase(kLeft)
  goto loop

YIELD:
  current = LoadCurrentNode()
  value = current.value
  YieldValue(value, original_frame_state)
  SavePhase(kRight)
  goto loop

RIGHT:
  current = LoadCurrentNode()
  right = current.right
  if original right guard proves no child:
      SavePhase(kBacktrack)
  else if right is not an exact supported Node:
      fallback/deopt or reject optimization
  else:
      SaveCurrentNode(right)
      SavePhase(kLeft)
  goto loop

BACKTRACK:
  if LoadStackTop() == 0:
      return Py_None
  node = StateStackPop()
  phase = LoadPoppedPhase()
  SaveCurrentNode(node)
  SavePhase(phase)
  goto loop
```

注意：进入左子树时 push parent 的 phase 是 `kYield`，不是 `kRight`。左子树结束后需要先 yield parent value，再处理右子树。

### 9.2.5 codegen 算法

状态机 LIR opcode 的热路径不走 C runtime helper。首版通过 frame pointer 正偏移读取 `GenDataFooter.tree_iter_state` 指针，再访问 heap state 字段：

```text
state = *(TreeIterState**)(FP + offsetof(GenDataFooter, tree_iter_state))
state field address = state + offsetof(TreeIterState, field)
```

基础读写：

```text
LoadPhase:
  state = footer.tree_iter_state
  output = state.current_phase

SavePhase:
  state = footer.tree_iter_state
  state.current_phase = input

LoadStackTop:
  state = footer.tree_iter_state
  output = state.stack_top

LoadPoppedPhase:
  state = footer.tree_iter_state
  output = state.popped_phase
```

对象读写：

```text
LoadCurrentNode:
  state = footer.tree_iter_state
  output = state.current_node
  if output != nullptr:
      Py_INCREF(output)

SaveCurrentNode:
  state = ensureTreeIterState(footer)
  old = state.current_node
  if old != nullptr:
      Py_DECREF(old)
  if input != nullptr:
      Py_INCREF(input)
  state.current_node = input
```

栈操作：

```text
StateStackPush(node, phase):
  state = footer.tree_iter_state
  top = state.stack_top
  if top >= state.stack_capacity:
      if !growTreeIterStateStack(state):
          raise MemoryError on controlled slow path
  state.stack[top].node = node
  state.stack[top].phase = phase
  Py_XINCREF(node)
  state.stack_top = top + 1

StateStackPop:
  state = footer.tree_iter_state
  top = state.stack_top - 1
  assert top >= 0 in debug builds
  state.stack_top = top
  output = state.stack[top].node
  state.popped_phase = state.stack[top].phase
  clear state.stack[top]
  return output
```

`StateStackPop` 不对 `output` 额外 `INCREF`，因为栈持有的引用转移给返回寄存器，后续 `RefcountInsertion` 的 `XDecref` 负责释放。

`safe_overflow_path` 是 release 必需路径，不是 debug 断言。V1.2 固定选择可增长 heap stack：

| 项目 | 要求 |
| ---- | ---- |
| 初始容量 | 16 个 `TreeIterStackEntry`，覆盖旧分支主要验证深度 |
| 扩容触发 | 每次 push 前比较 `stack_top` 和 `stack_capacity` |
| 扩容方式 | 调用 runtime/helper 重新分配 stack buffer，复制旧条目，保持每个 owned `PyObject*` 引用不变 |
| 扩容失败 | 设置 `MemoryError` 或等价受控异常，不能写越界，不能泄漏已 owned 引用 |
| GC/clear | `visitTreeIterState` 与 `clearTreeIterState` 按 `[0, stack_top)` 遍历动态 stack |

不得在 release 构建中只依赖 `JIT_DCHECK`，也不得退回“普通动态树无法证明深度所以全部 no-op”的固定栈策略。

## 9.3 行为模型

### 9.3.1 正常流程

```text
1. 用户代码创建满足准入条件的 Tree/Node 对象，调用 list(tree) 或 for 循环。
2. JIT 编译 Node.__iter__。
3. TreeIterStateMachinePass 在 HIR 优化阶段识别目标模式、原始空子树 guard、exact type/layout guard 和实验/生产 gate 条件。
4. pass 生成状态机 CFG，并移除原始递归 yield-from 路径。
5. 生成器首次执行 InitialYield 前保存 self 和初始 phase。
6. 每次 resume 后进入 bb_loop，根据 current_phase 分派。
7. 状态机通过 `GenDataFooter.tree_iter_state` 指向的 heap state 持久化 current_node、phase 和 stack。
8. YIELD 阶段暂停 generator；首版实验只声明 `next()`/`for` 消费结果正确，生产路径在 yield-from 可观察协议不可保留时不触发优化。
9. BACKTRACK 阶段栈为空时返回 None，generator 结束。
```

### 9.3.2 异常流程

| 场景 | 处理 |
| ---- | ---- |
| HIR 模式不匹配 | pass 直接 return，保留原始 HIR |
| 缺少 `left/right/value` 任一字段 offset | pass 直接 return |
| 找不到 `InitialYield`、`self_reg` 或普通 `YieldValue` FrameState | pass 直接 return |
| 缺少原始空子树 guard 且 child 可能为 None | pass 直接 return，保留 CPython TypeError 语义 |
| child 不是同一 exact Node 类型或 `__iter__` 可能被覆盖 | pass 直接 return |
| `gi_yieldfrom`、`send/throw/close`、StopIteration value 无法保持或精确 deopt | 生产配置 pass 直接 return；实验配置只能覆盖已声明的 `next()`/`for` 受控场景 |
| 生成状态机中途失败 | 不允许留下半改写 CFG；实现应先完成匹配和字段收集，再一次性改写 |
| `state_stack` 溢出 | release 必须在写入前进入 heap stack grow 路径；扩容失败走受控错误路径 |
| 对象 refcount 降为 0 | `SaveCurrentNode` 或 codegen decref 路径必须调用 `_Py_Dealloc`，不得只写回负 refcount |
| deopt 请求 | 生产配置必须满足 9.6.4 的 deopt contract；首版实验不声明 production deopt，生产 gate 未闭合时禁用优化 |
| 目标架构无 codegen | 编译期 `CINDER_UNSUPPORTED` 或配置关闭 |

## 9.4 数据模型

### 9.4.1 数据结构定义

#### 9.4.1.1 `TreeIterMatch`

新增内部结构，建议定义在 `tree_iter_state_machine_pass.h`：

```cpp
struct TreeIterMatch {
  Register* self_reg{nullptr};
  Instr* initial_yield{nullptr};
  FrameState* yield_frame_state{nullptr};
  Type exact_node_type{TTop};

  std::size_t left_offset{0};
  std::size_t right_offset{0};
  std::size_t value_offset{0};

  Instr* left_none_guard{nullptr};
  Instr* right_none_guard{nullptr};
  const YieldValue* left_yield_from{nullptr};
  const YieldValue* value_yield{nullptr};
  const YieldValue* right_yield_from{nullptr};

  bool can_preserve_yield_from_protocol{false};
  bool can_deopt_state_machine{false};
};
```

`left_yield_from` 和 `right_yield_from` 使用当前 master 的 `YieldValue` 表示，而不是旧分支的 `YieldFrom`。

#### 9.4.1.2 `TreeIterPhase`

```cpp
enum class TreeIterPhase : int32_t {
  kLeft = 0,
  kYield = 1,
  kRight = 2,
  kBacktrack = 3,
};
```

#### 9.4.1.3 `GenDataFooter` 与 `TreeIterState`

`GenDataFooter` 属于所有 JIT generator 的 suspend data。首版不得把固定 16 项 `state_stack` 直接内嵌进去，否则每个非 TreeIter JIT generator 都会增加 256B+ 内存面。V1.2 固定使用最小 footer 指针 + 按需 heap state：

```cpp
struct GenDataFooter {
  // existing fields...

  TreeIterState* tree_iter_state{nullptr};
};

struct TreeIterStackEntry {
  PyObject* node{nullptr};
  int32_t phase{0};
  int32_t reserved{0};
};

struct TreeIterState {
  PyObject* tree_iter_current_node{nullptr};
  int32_t tree_iter_current_phase{0};
  int32_t tree_iter_stack_top{0};
  int32_t tree_iter_stack_capacity{0};
  int32_t tree_iter_popped_phase{0};
  int32_t tree_iter_reserved{0};

  TreeIterStackEntry* tree_iter_stack{nullptr};
};
```

命名加 `tree_iter_` 前缀，避免与未来通用 generator 状态字段混淆。需要保留 entry 尺寸约束，但不再保留 footer 内嵌数组尺寸约束：

```cpp
static_assert(sizeof(TreeIterStackEntry) == 16);
```

内存影响 gate：

| 项目 | 要求 |
| ---- | ---- |
| 非 TreeIter generator | 仅增加一个指针字段，不承担 heap stack 分配 |
| TreeIter 实验路径 | 首次进入状态机前分配 `TreeIterState` 和初始 16 项 stack |
| free-list 复用 | generator 释放或 clear 时必须释放 heap stack 并将 footer 指针清空 |
| 生产默认 | 生产 gate 未闭合时不分配 `TreeIterState` |

### 9.4.2 数据流转

#### 9.4.2.1 编译期数据流

```text
HIR Function
  -> pass-local TreeIter matcher
  -> TreeIterMatch
  -> pass-local state-machine builder
  -> 状态机 HIR blocks
  -> LIR generator
  -> machine code
```

#### 9.4.2.2 运行期数据流

```text
LoadArg(0) self
  -> ensureTreeIterState(GenDataFooter)
  -> SaveCurrentNode(self)
  -> GenDataFooter.tree_iter_state->tree_iter_current_node
  -> LoadCurrentNode()
  -> LoadField(left/right/value)
  -> SaveCurrentNode(child) or StateStackPush(parent)
  -> YieldValue(value)
  -> resume
  -> LoadPhase()
```

#### 9.4.2.3 引用所有权流

```text
SaveCurrentNode(new):
  TreeIterState owns new reference

LoadCurrentNode():
  TreeIterState keeps its reference
  output register receives a new reference

StateStackPush(node):
  stack owns a new reference

StateStackPop():
  stack reference moves to output register
```

GC traverse 需要经 `footer->tree_iter_state` 访问 `tree_iter_current_node` 和 `tree_iter_stack[i].node`；clear/dealloc 需要 `Py_CLEAR` 所有这些字段并释放 heap stack。

## 9.5 接口设计

### 9.5.1 内部接口设计

```text
TreeIterStateMachinePass::Run(Function&)
  -> matchTreeIter(Function&) -> optional<TreeIterMatch>
  -> buildTreeIterStateMachine(Function&, TreeIterMatch&)
```

状态机 builder 不暴露给其它 pass。所有状态访问通过新增 HIR 指令表达，不在 pass 中直接生成 CallCFunc。

### 9.5.2 内部接口定义

#### 9.5.2.1 Pass 接口

```cpp
class TreeIterStateMachinePass : public Pass {
 public:
  TreeIterStateMachinePass() : Pass("TreeIterStateMachinePass") {}
  void Run(Function& func) override;

 private:
  std::optional<TreeIterMatch> matchTreeIter(Function& func) const;
  void buildTreeIterStateMachine(Function& func, const TreeIterMatch& match);
};
```

#### 9.5.2.2 HIR 指令接口

```cpp
DEFINE_SIMPLE_INSTR(SaveCurrentNode, (TObject), Operands<1>);
DEFINE_SIMPLE_INSTR(LoadCurrentNode, (TObject), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(SavePhase, (TCInt32), Operands<1>);
DEFINE_SIMPLE_INSTR(LoadPhase, (TCInt32), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(StateStackPush, (TObject, TCInt32), Operands<2>);
DEFINE_SIMPLE_INSTR(StateStackPop, (TObject), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(LoadPoppedPhase, (TCInt32), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(LoadStackTop, (TCInt32), HasOutput, Operands<0>);
```

输出类型：

| 指令 | 输出类型 |
| ---- | -------- |
| `LoadCurrentNode` | `TObject` |
| `StateStackPop` | `TObject` |
| `LoadPhase` | `TCInt32` |
| `LoadPoppedPhase` | `TCInt32` |
| `LoadStackTop` | `TCInt32` |

#### 9.5.2.3 LIR 接口

新增 LIR opcode：

```cpp
kSaveCurrentNode
kLoadCurrentNode
kSavePhase
kLoadPhase
kStateStackPush
kStateStackPop
kLoadPoppedPhase
kLoadStackTop
```

HIR lowering 必须使用 `appendInstr(Instruction::kXxx)` 或带 output 的 `appendInstr(output, Instruction::kXxx)`，不得使用 `appendCallInstruction`。旧分支已验证 `appendCallInstruction` 生成的是通用 `kCall`，不会触发 `BEGIN_RULES(Instruction::kXxx)` 的原生 translate 规则。

#### 9.5.2.4 配置接口

```cpp
struct HIROptimizations {
  ...
  bool tree_iter_state_machine{false};
};
```

环境变量：

```text
PYTHONJITTREEITERSTATEMACHINE=1
```

首版默认 `false`，只允许测试和 benchmark 显式开启实验路径。只有生产 gate 全部通过后，才能单独评估默认开启。

配置开启还必须受平台和语义 gate 约束：

```text
effective_enabled =
  tree_iter_state_machine
  && target_arch_verified
  && current_master_hir_admission_proven
  && heap_tree_iter_state_enabled
  && experimental_tree_iter_core_enabled
```

生产配置还必须满足：

```text
production_enabled =
  effective_enabled
  && generator_protocol_model_enabled
  && deopt_reify_model_enabled
  && lifecycle_matrix_passed
  && performance_gate_passed
```

实验配置任一 `effective_enabled` 条件不满足时，`TreeIterStateMachinePass` 必须 no-op，不能生成状态机 HIR。生产配置任一 `production_enabled` 条件不满足时必须 no-op；x86_64 未完成同等 correctness、protocol、deopt 和性能矩阵前，即使配置为 true 也必须禁用生产路径。

## 9.6 代码实现要点

### 9.6.1 pass 插入点

当前 pipeline 在 simplify、PrimitiveUnboxCSE、PrimitiveBoxRemat、CleanCFG、DCE 后运行 `RefcountInsertion`。状态机 pass 应插入到 `RefcountInsertion` 前，建议位置：

```text
runPassIf(CleanCFG)
runPassIf(DeadCodeElimination)
runPassIf(CleanCFG)
runPassIf(TreeIterStateMachinePass)
runPass(RefcountInsertion)
```

原因：

1. 状态机新指令需要让 `RefcountInsertion` 处理输出 object 的 `XDecref`。
2. pass 生成 CFG 后需要 cleanup 和 type reflow，避免新寄存器保持 `TTop`。
3. 如果状态机 pass 放在 `RefcountInsertion` 后，新增 object 输出不会获得统一引用计数处理。

### 9.6.2 `InitialYield` 前插入

实现必须显式定位 `InitialYield`：

```cpp
auto* init_block = initial_yield->block();
auto init_iter = iterator_pointing_to(initial_yield);

init_block->insert(SaveCurrentNode::create(self_reg), init_iter);
init_block->insert(LoadConst::create(init_phase, Type::fromCInt(kLeft, TCInt32)), init_iter);
init_block->insert(SavePhase::create(init_phase), init_iter);
```

`SaveCurrentNode` 首次执行时必须先确保 `footer->tree_iter_state` 已分配并初始化；实现可以把 lazy allocation 放在 `SaveCurrentNode` lowering/helper 中，或在 `InitialYield` 前显式插入 `EnsureTreeIterState`。不要使用会 append 的 `CreatePhaseConst()` 来创建 `init_phase`。旧分支记录过该错误会导致 `SavePhase(init_phase)` 在定义前执行。

### 9.6.3 空子树 guard 和 `nullptr` 检查

状态机不得把裸 `yield from None` 改写为空遍历。空子树分支只能来自原始 HIR 已存在的 guard：

```text
if original guard proves child is None:
    no_child
else if child is nullptr from Static Python/internal layout and original semantics treats it as absent:
    no_child
else if child exact type guard succeeds:
    has_child
else:
    fallback/deopt or reject optimization
```

普通 Python 对象字段中的 `Py_None` 只有在原始源码/HIR 已显式跳过 `None` 时才能走 `no_child`。`nullptr` 仅用于 Static Python 或内部布局确有 nullable field 语义的场景，必须由类型系统和原始控制流共同证明。

### 9.6.4 引用清理

新增 helper：

```cpp
void clearTreeIterState(GenDataFooter* footer);
int visitTreeIterState(GenDataFooter* footer, visitproc visit, void* arg);
```

职责：

```text
clearTreeIterState:
  state = footer.tree_iter_state
  if state == nullptr: return
  Py_CLEAR(state.tree_iter_current_node)
  for entry in state.tree_iter_stack[0:state.stack_top):
      Py_CLEAR(entry.node)
      entry.phase = 0
  free state.tree_iter_stack
  free state
  footer.tree_iter_state = nullptr

visitTreeIterState:
  state = footer.tree_iter_state
  if state == nullptr: return
  Py_VISIT(state.tree_iter_current_node)
  for i in [0, state.stack_top):
      Py_VISIT(state.tree_iter_stack[i].node)
```

调用点：

| 调用点 | 要求 |
| ------ | ---- |
| `JITRT_AllocateAndLinkGenAndInterpreterFrame` | 初始化 `tree_iter_state` 指针为空 |
| `jitgen_traverse` | visit 当前节点和栈节点 |
| `deopt_jit_gen` | 首版实验路径遇到 active TreeIter state 且无精确 reify 时不得声明成功生产 deopt；生产配置必须 no-op |
| `gen_dealloc_with_custom_free` 或其前置路径 | free-list 复用前释放引用并清零 |
| generator 正常结束路径 | 状态机 done 前或 frame clear 时释放 current/stack 引用 |

V1.2 固定 deopt 决策：首版实验实现不提供 production deopt contract；生产 gate 在精确 reify 未实现前禁用状态机。后续生产化必须选择并实现精确 reify，不能再以“不可 deopt 状态机”作为普通动态 generator 的隐含假设。

后续精确 reify 设计必须在 contract 表中逐项定义引用转移顺序：

| 路径 | 必须说明 |
| ---- | -------- |
| 正常完成 | done block 何时 `clearTreeIterState`，以及与 `jitFrameClearExceptCode` 的先后 |
| `close()` | 是否先精确 deopt 并委派给 CPython，或如何向当前 delegated iterator 注入 `GeneratorExit` |
| `throw()` | 是否先精确 deopt 并委派给 CPython，或如何按 `_gen_throw` 语义传播 |
| instrumentation attach | `jitgen_am_send_with_deopt` / 全量 suspended generator deopt 如何处理状态机 footer |
| tp_clear/finalize/dealloc | GC 清理时哪些字段仍 owned，哪些已转移给解释器 |

### 9.6.5 `instr_effects` 策略

首版保守设置：

| 指令 | memoryEffects | hasArbitraryExecution |
| ---- | ------------- | --------------------- |
| `SaveCurrentNode` | `AOther` | true |
| `LoadCurrentNode` | `AEmpty` 或 `AOther` | true，因为包含 INCREF |
| `SavePhase` | `AOther` | true |
| `LoadPhase` | `AEmpty` | false |
| `StateStackPush` | `AOther` | true |
| `StateStackPop` | `AOther` | true |
| `LoadPoppedPhase` | `AEmpty` | false |
| `LoadStackTop` | `AEmpty` | false |

旧分支证明不能把 `SavePhase` 标成无副作用，否则优化器可能消除关键状态转换，导致只遍历左叶节点。

### 9.6.6 generator 协议模型

CPython yield-from 的可观察行为不等同于只产出相同值序列。当前 CinderX runtime 通过 `YieldValue::isYieldFrom()` 生成 `StoreGenYieldFromPoint`，并在 suspend 后设置 `FRAME_SUSPENDED_YIELD_FROM`，供 `gi_yieldfrom`、`close()` 和 `throw()` 路径使用。状态机若改用普通 `YieldValue`，必须补足等价模型。

V1.2 固定首版协议决策为“实验限制”，不再把三种互斥方案留给实现阶段：

| 模型 | 首版状态 | 要求 |
| ---- | -------- | ---- |
| 实验限制 | 采用 | 默认关闭；只声明 `next()`/`for`/`list()` 受控消费结果正确；不声明 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、StopIteration value 委派语义生产完备 |
| 保留 delegated iterator 元数据 | 不采用 | 后续若采用，必须定义 suspend 时 `gi_yieldfrom` 返回的具体对象、显式栈上的委派链和 StopIteration value 传播 |
| 精确 deopt 后委派 | 生产化推荐方向，但非首版 | 后续必须定义 `TreeIterState` 到等价解释器 generator/yield-from 栈的 reify 格式，再调用 CPython 原路径 |

生产配置在后两种模型之一实现前必须 no-op。实验测试不得用 “`list(tree)` 正确” 推导 production-ready，只能记录实验性能和受控 correctness。

### 9.6.7 codegen 寄存器约束

AArch64 codegen 注意事项：

1. `arch::ptr_resolve()` 会 clobber scratch register，不能把同一个 scratch 同时用于保存 `stack_top`。
2. 只使用 `DISALLOWED_REGISTERS` 范围的 scratch clobber 临时值，避免覆盖寄存器分配器分配的活跃值。
3. `SaveCurrentNode` decref 可能调用 `_Py_Dealloc`，该 LIR 指令必须被 postalloc move fold 视为调用或 clobber 屏障。
4. `StateStackPush` 需要先把输入 node/phase 读入安全临时寄存器，再加载 `stack_top`，避免输入分配到同一寄存器被覆盖。

x86_64 codegen 注意事项：

1. 可使用 `rbp + offsetof(...)` 直接寻址。
2. 固定寄存器如 `rax/rdi/rsi/r13` 的使用必须符合当前 codegen scratch 约定。
3. x86_64 分支不得仅因编译通过而标记支持，必须跑同等 correctness 测试。

### 9.6.8 postalloc 约束

新增原生 LIR 指令后，`optimizeMoveSequence` 不能只以 `isCall()` 作为扫描边界。至少需要满足：

1. 扫描 fold 区间时检查中间寄存器是否被显式输入使用。
2. 检查 memory indirect operand 的 base/index 寄存器。
3. 将可能调用 `_Py_Dealloc` 的 `SaveCurrentNode` 视为 clobber 屏障。

否则可能删除 `Move X19, X0` 这类保存 call 返回值的 move，导致后续 `SaveCurrentNode X19` 使用 stale register。

### 9.6.9 测试实现要点

新增测试建议放入 `cinderx/PythonLib/test_cinderx/test_jit_generators.py`：

| 测试 | 内容 |
| ---- | ---- |
| `test_tree_iter_state_machine_depths` | depth 1-12 结果正确 |
| `test_tree_iter_state_machine_repeated_iteration` | 同一棵树重复 `list(tree)` |
| `test_tree_iter_state_machine_guarded_none_children` | 原始源码带 `is not None` guard 时，None 子树被跳过且状态机可触发 |
| `test_tree_iter_state_machine_bare_yield_from_none_not_optimized` | 裸 `yield from None` 保留 CPython `TypeError`，不得被状态机跳过 |
| `test_tree_iter_state_machine_non_iterable_child_not_optimized` | 非 iterable child 保留原始异常 |
| `test_tree_iter_state_machine_exact_type_required` | 子类覆盖 `__iter__`、自定义 iterator、descriptor/property 副作用不触发 |
| `test_tree_iter_state_machine_not_triggered_for_non_tree` | 非目标 generator 不触发 |
| `test_tree_iter_state_machine_disabled` | 关闭环境变量时不触发 |
| `test_tree_iter_state_machine_stack_limit` | depth 16、17、极深 skewed tree 在 release 下触发 heap stack 扩容，不越界、不崩溃 |
| `test_tree_iter_state_machine_gc_cycle` | current/stack 持有引用时 GC 不泄漏、不悬挂 |
| `test_tree_iter_state_machine_protocol_gate` | 首版实验路径不声明 `gi_yieldfrom`、`send(non-None)`、`throw`、`close` 生产支持；生产配置在协议 gate 未闭合时 no-op |
| `test_tree_iter_state_machine_deopt_gate` | suspended generator deopt / instrumentation attach gate 未闭合时生产配置 no-op |
| `test_tree_iter_state_machine_arch_gate` | 未验证架构配置开启仍 no-op |
| `test_tree_iter_state_machine_hir_admission_sample` | pyperformance 或本地 TreeIter 样例的当前 master HIR 满足 owner/child/iterator identity 准入；不满足时不声明覆盖 pyperformance |

测试必须显式启用：

```text
PYTHONJIT=1
PYTHONJITTREEITERSTATEMACHINE=1
```

性能测试必须使用 jitlist 或明确调用 `cinderx.jit.compile_after_n_calls()`，不能只设置低 `PYTHONJITAUTO` 阈值；旧分支证明低阈值可能编译 stdlib 函数并引入无关崩溃。

生产 gate 测试矩阵：

| 维度 | 要求 |
| ---- | ---- |
| correctness | debug/release 均跑 guarded None、bare None、非 iterable、exact type、子类覆盖、深度边界 |
| protocol | 生产启用前覆盖 `gi_yieldfrom`、`send`、`throw`、`close`、StopIteration value、异常上下文；首版实验只断言生产 gate no-op |
| lifecycle | 覆盖 GC cycle、tp_clear、finalize、free-list 复用、正常完成、异常退出 |
| deopt | 生产启用前覆盖 suspended generator deopt、instrumentation attach、deopt 后继续执行；首版实验只断言生产 gate no-op |
| platform | 每个启用架构独立通过；未验证架构 no-op |
| performance | 指定 benchmark 命令、重复次数、统计阈值和非目标 generator 回退阈值 |

# 10 DFX分析

## 10.1 可靠性分析

| 风险 | 详细设计约束 |
| ---- | ------------ |
| 状态字段隐藏引用导致 GC 漏扫 | `jitgen_traverse` 访问 current node 和 stack node |
| free-list 复用旧状态 | 分配时初始化，释放时 clear |
| borrowed ref 被 `XDecref` 释放 | Load 返回前 `INCREF`，Pop 转移 owned reference |
| `InitialYield` clobber self | 初始化保存插入到 `InitialYield` 前 |
| HIR 优化破坏状态转换 | 写状态和 refcount 指令保守副作用 |
| 原生 LIR 被 postalloc 错误 fold | 增加 clobber 屏障和中间寄存器使用检查 |
| 裸 `yield from None` 语义漂移 | 只匹配原始 guard；裸 yield-from 保留 `GetIter`/TypeError |
| generator 协议漂移 | 首版实验不声明生产支持；生产启用前必须保留 delegated iterator 元数据或在可观察前精确 deopt |
| release 栈溢出 | push 前动态检查并扩容 heap stack，扩容失败走受控错误路径 |

## 10.2 异常处理设计

状态机首版只优化无额外异常语义的 TreeIter 实验模式；生产启用必须另行闭合协议和 deopt：

1. `LoadField(left/right/value)` 仍保留原 HIR 的 guard/check 语义，且不得删除 `GET_YIELD_FROM_ITER` 中仍可观察的 TypeError/coroutine rejection 语义。
2. 首版实验 `YieldValue` 不声明 yield-from 可观察协议等价；生产配置在 `gi_yieldfrom`、`send/throw/close` 可观察语义未闭合前 no-op。
3. 对无法精确恢复的 deopt 场景，生产配置应拒绝优化；实验配置必须通过 gate 测试证明不会被误标为生产可用。
4. 对 `_Py_Dealloc` 罕见路径，codegen 可以调用 C helper；该指令必须标记为 clobber 边界。
5. 栈 underflow/overflow 在 release 构建必须有 heap-backed 动态安全路径；debug `JIT_DCHECK` 只能作为额外诊断。

## 10.3 性能分析

性能收益来自三点：

| 来源 | 说明 |
| ---- | ---- |
| 消除递归 generator yield-from 帧切换 | 不再为 left/right 子树递归创建和恢复 generator 帧 |
| Heap-backed TreeIterState | footer 只保存状态指针，current/phase/stack 通过一次指针解引用访问 |
| 原生 LIR/codegen | 避免热路径 C runtime helper 调用 |

首版实验性能验收口径：

```text
baseline: 当前 master + PYTHONJITTREEITERSTATEMACHINE=0
target:   当前 master + PYTHONJITTREEITERSTATEMACHINE=1（实验路径）
case:     已通过当前 master HIR 准入证明的 pyperformance generators Tree.__iter__；
          若证明失败，则使用等价 depth 1-12 micro benchmark，不能宣称覆盖 pyperformance
```

旧分支记录的 4-12x 只作为预期参考，重实现必须重新在当前 master 上测量。提交前必须把性能 gate 固化为可复现命令和阈值，例如：

| Gate | 要求 |
| ---- | ---- |
| 目标收益 | 实验 target 相对 baseline 达到预设加速阈值；生产收益 gate 需在协议/deopt 闭合后重测 |
| 非目标回退 | 非 TreeIter generator benchmark 无统计显著回退 |
| 稳定性 | 多轮重复的均值、方差和异常值处理方式固定 |
| 口径 | 明确 CPython/CinderX、JIT on/off、feature on/off，不混用口径基线和提交基线 |

## 10.4 安全和韧性分析

本功能不新增外部权限、文件、网络或用户输入解析。安全重点是 native 内存安全：

1. `state_stack` 访问必须有容量边界和 heap 扩容失败路径。
2. `GenDataFooter.tree_iter_state` 必须初始化为空，避免读未初始化指针。
3. 所有 `PyObject*` 状态必须参与 GC traverse 和 clear。
4. 架构相关 codegen 默认只在已验证平台开启。
5. 配置项保留快速关闭能力，便于线上回退和性能二分。
6. CPython generator/yield-from 可观察协议必须作为生产 correctness 边界，而不是性能测试附属项；首版实验结果不得替代生产 gate。
