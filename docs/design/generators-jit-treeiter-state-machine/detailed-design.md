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
| V1.3 | 2026-05-28 | Codex Agent | 修复二轮审查剩余 gated_auto，补齐状态 op 异常契约、真实 workload gate 和人工决策清单 |
| V1.4 | 2026-05-28 | Codex Agent | 同步功能设计决策，明确生产协议 exact deopt/reify、active-path 环检测、原始递归深度边界和五个纵切面准入 artifact |
| V1.5 | 2026-05-28 | Codex Agent | 修复文档审查意见，拆分实验/生产 gate，补齐 reify 可表示性、active-path/depth 转移不变量、artifact/workload schema 和实验协议 guard |
| V1.6 | 2026-05-29 | Codex Agent | 按性能根因分析重设 V2 详细设计：引入 `FieldAccessProof`、truthiness 等价性、split-dict 字段 guard/deopt，并补充 21 个 JIT 用例覆盖边界 |
| V1.7 | 2026-05-29 | Codex Agent | 修复 V2 审校意见：明确 proof 载荷穿透 HIR/LIR、动态 child guard、实验 fail-closed 动作、生产 artifact 信任边界和 `generators` 硬 gate |

## 4 Keywords 关键词

CinderX, JIT, generator, yield from, TreeIter, HIR, LIR, GenDataFooter, FieldAccessProof, split-dict, state machine, refcount, AArch64, postalloc

## 5 Abstract 摘要

本文档描述基于当前 `master` 重新实现 generators JIT TreeIter 状态机优化的详细设计。输入来自上一阶段功能设计文档 `docs/design/generators-jit-treeiter-state-machine/function-design.md`，实现证据来自旧分支 `github/bench-cur-7c361dce-claudecode` 中已完成的 `TreeIterStateMachinePass`、`GenDataFooter` 扩展、状态机 HIR/LIR 指令、AArch64/x86_64 codegen 和调试经验。旧分支仅作为 MVP 参考；正确性以当前 CinderX 源码、该构建绑定的 CPython source tag/commit，以及 CI/local artifact 中记录的源码获取方式为准。本地绝对路径不能作为规范来源。

本次详细设计不按旧分支逐文件复制。旧分支的核心算法保留，但接口适配当前 `master`：当前 HIR 使用 `Send` 与 `YieldValue::yieldFromIter()` 表示 yield-from，不再引入旧分支的显式 `YieldFrom`、`OptimizedYieldFrom`、`InlineIter` 作为必要前置。V1.2 将首版收敛为默认关闭的实验核心实现：选择 heap-backed growable `TreeIterState` 解决 release 栈溢出和全局内存膨胀问题；不把 `gi_yieldfrom` / `send` / `throw` / `close` / suspended deopt 精确协议声明为首版生产能力。V1.5 将实验 gate 和生产 gate 分开：实验路径只证明 allowlisted `next()`/`for`/`list()` 核心正确性和性能，并在协议敏感入口 fail closed；生产路径必须先证明 TreeIterState 能 exact reify 为等价原始 generator/yield-from 栈，再闭合 active-path/depth 转移不变量、五个纵切面 artifact verifier 和 `generators` workload gate。V1.6 针对当前实现未能提升 pyperformance `generators` 的根因，将 matcher 从“slot-only `LoadField` + 显式 `is not None`”扩展为“原始 child guard + 字段访问证明”：支持 default truthiness 下的 `if child:`，并支持 dict-backed heap object 的 split-dict fast path、fallback 语义和 guard/deopt 处理。

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

树遍历生成器通常写作显式 `None` guard：

```python
def __iter__(self):
    if self.left is not None:
        yield from self.left
    yield self.value
    if self.right is not None:
        yield from self.right
```

也常写作 truthiness guard：

```python
def __iter__(self):
    if self.left:
        yield from self.left
    yield self.value
    if self.right:
        yield from self.right
```

普通 JIT generator 路径会保留 Python generator 的递归帧切换和 yield-from 恢复机制。本文档的状态机只允许在原始源码/HIR 已经包含空子树跳过 guard，且能证明该 guard 语义等价于 `child is not None` 时启用。显式 `is not None` guard 可以直接提供该语义；truthiness guard 还必须证明 child 只可能是 `None` 或同一 exact 节点类型，且该类型使用默认 truthiness、没有自定义 `__bool__` / `__len__`。裸 `yield from self.left/right` 不具备 `None` 为空的语义；按 CPython `GET_YIELD_FROM_ITER`，`yield from None` 必须抛 `TypeError`。

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

1. 当前 `master` 中如何识别带 child 跳过 guard、truthiness 等价性和精确类型约束的 TreeIter yield-from HIR。
2. 如何用 `FieldAccessProof` 覆盖 slot/member 字段和 dict-backed split-dict 字段访问，并让 proof 载荷或 proof id 穿透 HIR、printer/parser、LIR 和 codegen，保留 fallback、invalidation 或 deopt 语义。
3. 新增哪些内部数据结构、HIR 指令、LIR 指令和 codegen 规则。
4. 状态机 CFG 如何在显式实验配置下生成，以及哪些语义不允许声明为生产能力。
5. `PyObject*` 引用如何在 `GenDataFooter`、heap-backed `TreeIterState`、寄存器和显式栈之间转移。
6. 首版如何闭合 release 栈容量安全、GC/finalize/tp_clear；生产 deopt、`gi_yieldfrom`、`send/throw/close`、环检测和深度越界由 exact deopt/reify gate 承接。
7. 需要在哪些现有文件中接入配置、初始化、GC、deopt、测试和性能验证。

# 8 上游文档引用

| 类型 | 路径或来源 | 用途 |
| ---- | ---------- | ---- |
| 功能设计 | `docs/design/generators-jit-treeiter-state-machine/function-design.md` | 功能边界、DFX、验收规格 |
| 当前 master HIR builder | `cinderx/Jit/hir/builder.cpp` | `YieldValue::setYieldFromIter()` 生成逻辑 |
| 当前 master HIR 定义 | `cinderx/Jit/hir/hir.h` | `YieldValue`、`Send`、`InitialYield` 当前接口 |
| 当前 master 字段访问 HIR | `cinderx/Jit/hir/hir.h`、`cinderx/Jit/hir/builder.cpp`、HIR dump artifact | `LoadField`、`LoadAttr`、`CheckField`、split-dict inline-values valid guard 和合流形态 |
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
| `cinderx/Jit/hir/tree_iter_state_machine_pass.h` | 新增 pass、匹配结果、`FieldAccessProof`、状态机生成器声明 |
| `cinderx/Jit/hir/tree_iter_state_machine_pass.cpp` | 新增模式识别、truthiness 等价性验证、字段访问证明提取、CFG 生成、回退逻辑 |
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
| `cinderx/Jit/codegen/autogen.cpp` | 首版新增 AArch64 translate 函数和规则；x86_64 首版只保持 no-op arch gate |
| `cinderx/PythonLib/test_cinderx/test_jit_generators.py` | 新增 TreeIter correctness、触发、回退测试 |

构建系统当前通过 `CMakeLists.txt` 中 `file(GLOB_RECURSE JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/*.cpp ${PROJECT_SOURCE_DIR}/Jit/*.c)` 收集 JIT 源文件，因此新增 `Jit/hir/tree_iter_state_machine_pass.cpp` 不需要手工加入源列表。

### 9.1.2 实现层次

```text
当前 master HIR
  guard(left/right is not None or default-truthiness if child),
  YieldValue(isYieldFrom=true), Send,
  GET_YIELD_FROM_ITER CFG, LoadField / split-dict LoadField+CheckField+LoadAttr Phi
        |
        v
pass-local TreeIter matcher
  TreeIterMatch{self, exact node type, left/right/value FieldAccessProof,
                child guard proof, frame state, initial yield}
        |
        v
pass-local state-machine builder
  生成 Ensure/ClearTreeIterState、Load/SaveCurrentNode、Load/SavePhase、StateStackPush/Pop、YieldValue
        |
        v
LIR lowering
  生成 kEnsureTreeIterState/kLoadPhase/kSavePhase/kLoadCurrentNode/kSaveCurrentNode/...
        |
        v
Codegen
  通过 FP + offsetof(GenDataFooter, tree_iter_state) 读取状态指针，
  再访问 heap-backed TreeIterState 字段
```

### 9.1.3 首版交付闭环和生产边界

首版不是 production-ready 默认启用方案，而是显式实验交付。二轮审校和功能设计决策后固定如下闭环，避免实现阶段继续在互斥模型之间摇摆：

| 主题 | 决策 | 结果 |
| ---- | --------- | ---- |
| 交付定位 | 默认关闭的实验核心，面向 `next()` / `for` / `list()` 受控消费 | 不声明完整 generator 协议生产完备 |
| release 栈容量 | 选择 heap-backed growable `TreeIterState`，初始容量 16，push 前检查并在语义深度预算内扩容 | 不再使用固定 16 项 footer 内嵌栈作为 release 边界，也不隐式支持超过原始递归 generator 语义的深度 |
| footer 内存影响 | `GenDataFooter` 只新增一个 `TreeIterState*` 指针 | 非 TreeIter JIT generator 不承担 256B+ 固定栈开销 |
| 环检测 | 生产路径维护当前递归路径 active-path 集合，检测回边；不做全图 visited 去重 | self-cycle/right-cycle 等回边 exact deopt/reify；shared subtree 离开当前路径后可再次进入 |
| yield-from 协议 | 生产路径在 `gi_yieldfrom` / `send(non-None)` / `throw` / `close` 等协议敏感操作前 exact deopt/reify | 无法 reify 时生产配置 no-op；不再保留 delegated metadata 作为首选生产方案 |
| suspended deopt | 生产路径必须实现 footer state 到等价解释器 generator/yield-from 栈的 exact reify | instrumentation、`close()`、`throw()` 的生产路径在 reify 未闭合前禁用状态机 |
| 实验准入 | 必须有精确 code-object allowlist/jitlist 或专用 harness、当前 HIR 形态证据、exactness/layout/iterator identity 实验证据和受控协议入口 | 缺少 allowlist、wildcard jitlist 或低 `PYTHONJITAUTO` 阈值时 no-op；实验结果不能替代生产 gate |
| 生产准入 | 必须提供五个纵切面准入 artifact verifier：HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt | 缺任一 artifact 或 verifier 过期时生产 no-op；CI/release 证据进入发布清单，不作为普通 runtime boolean |
| 字段证明 | 使用 `FieldAccessProof` 同时描述 slot/member 和 dict-backed split-dict 字段访问 | TreeIter 使用该证明读取 `left/right/value`；后续 OO-heavy 优化可复用 exactness/layout/deopt 语义 |
| workload 覆盖 | M1.5 以 pyperformance `generators` 为直接性能目标和 M4 成功硬 gate，同时记录 21 个 JIT 用例的非目标/rollout 回归边界 | 未满足来源、形态分布、命令、allowlist、运行次数和稳定性标准时停止在 matcher/实验研究；不得把 `generators` benchmark 名称写入 matcher 特判 |

因此本文后续的状态机 CFG、HIR/LIR/codegen 描述属于 M0-M4 默认关闭实验路径。M5 生产 rollout 不与 V2 实验核心捆绑交付；只有 M4 证明 `generators` 收益和非目标回退 gate 达标后，才进入 M5 投资决策。任何生产启用必须实现 exact deopt/reify、active-path/深度语义、五个纵切面准入 artifact，并通过 9.6.6 和测试矩阵。

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
     - left/right 的原始 child guard blocks，包括 `is not None` guard 或 truthiness guard
  4. 对每个 yield-from YieldValue：
     - iter_reg = yv.yieldFromIter()
     - 追踪 iter_reg 的 producer，兼容 emitGetYieldFromIter 的 Assign/GetIter/Phi 合流
     - 保留或证明不需要 coroutine rejection 和 GetIter TypeError 语义
     - 最终 iterable 来源必须能形成 FieldAccessProof(base=current/self, name in {left, right})
  5. 验证每个 left/right yield-from 前存在原始 child skip guard：
     - `is not None` 等价 guard 可直接使用；
     - truthiness guard 必须额外证明 child 只可能是 None 或同一 exact Node，且 Node 使用 default truthiness。
     如果没有 guard，则 child 必须被证明为非 None 且同一 exact Node iterator。
  6. 对普通 YieldValue：
     - value_reg = yv.reg()
     - 最终必须追溯到 FieldAccessProof(name=value)
     - value 字段必须来自同一 exact Node 布局，并保留 slot/member 或 split-dict 的 guard/deopt 语义
  7. 检查 yield-from 字段顺序为 left, right，普通 yield 位于二者之间。
  8. 提取 `FieldAccessProof`、原 YieldValue FrameState、InitialYield block、实验/生产 gate 能力标志。
```

`traceYieldFromIterable(iter_reg)` 需要兼容当前 master 和旧分支已验证的形态：

```text
iter_reg.def == GetIter
iter_reg.def == Phi(..., GetIter, ...)
iter_reg.def == Phi(..., Assign(original_iterable), GetIter(original_iterable), ...)
iter_reg.def == Assign(original_iterable)
original_iterable.def == Phi(fast CheckField/LoadField arm, slow LoadAttr arm)
```

若 trace 结果需要删除 coroutine TypeError 或 non-iterable TypeError，则返回 no match，不修改 CFG。若生产配置无法在 `gi_yieldfrom`、`send(non-None)`、`throw`、`close` 等协议敏感操作前 exact deopt/reify，也必须 no-op；首版实验配置仅允许在已声明的 `next()`/`for`/`list()` 受控范围内生成状态机。

### 9.2.2 字段访问证明规则

字段追踪输出 `FieldAccessProof`，而不是裸 offset。该结构把“如何读取字段”和“字段证明失效时如何处理”绑定在一起，避免状态机只复制 fast `LoadField` 而删除当前 HIR 中仍可观察的 fallback 语义。

```cpp
enum class FieldAccessKind {
  kSlotOrMember,
  kSplitDict,
};

enum class RuntimeFailureAction {
  kExperimentalFailClosed,
  kInvalidate,
  kExactDeoptReify,
};

struct FieldAccessProof {
  FieldAccessKind kind;
  BorrowedRef<PyTypeObject> owner_type;
  std::string field_name;
  intptr_t value_offset;
  std::optional<intptr_t> valid_offset;
  GuardSource guard_source;
  DependencySource layout_dependency;
  FallbackShape fallback_shape;
  RuntimeFailureAction runtime_failure_action;
};
```

slot/member 字段追踪使用保守 unwrap：

```text
traceSlotOrMemberField(reg):
  instr = reg->instr()
  while instr is one of allowed transparent checks:
      instr = instr.primary_object_operand()->instr()
  if instr is LoadField with stable owner layout:
      return FieldAccessProof(kind=slot_or_member,
                              field_name, value_offset,
                              layout_dependency, runtime_failure_action)
  return none
```

split-dict 字段追踪必须识别 fast path 与 fallback 的合流，而不是只接受单个 `LoadField`：

```text
traceSplitDictField(reg):
  if reg is Phi(fast_value, slow_value):
      fast_value must trace to CheckField(field_name, LoadField(value_offset, base))
      fast block must be guarded by LoadField(inline_values.valid, base) or equivalent layout-valid check
      slow_value must trace to the original LoadAttr(field_name, base) fallback
      return FieldAccessProof(kind=split_dict,
                              field_name, value_offset, valid_offset,
                              guard_source, layout_dependency,
                              fallback_shape=LoadAttr fallback,
                              runtime_failure_action)
  return none
```

首版允许的透明检查只包含当前代码中明确可证明不改变对象身份的节点，例如 `CheckField`。不得把可能执行 Python 代码或读取用户可变状态的节点加入白名单。split-dict proof 的 `fallback_shape` 只用于证明原始语义和生产 deopt/reify 目标；状态机热路径不能无条件复制 fallback，因为 fallback 可能执行用户可见的 attribute protocol。

字段访问证明成功后还必须满足类型、truthiness 和 iterator identity：

| 条件 | 要求 |
| ---- | ---- |
| owner type | `self` 对应 type 必须可解析为当前 code object 的 owner method；Static/sealed 类型可由布局证明提供 exactness，普通 heap type 必须保留 `CheckExact`/type-version guard，不能只因为存在子类就整体排除目标 benchmark |
| child type | 每次动态读取非空 `left/right` 后，都必须 guard 为同一 exact owner type，或字段类型系统能证明不会是任意 iterable；observed local value 只能作为候选信号 |
| child guard | `is not None` guard 可直接作为空子树语义；truthiness guard 必须证明 exact Node 使用 default truthiness，且没有自定义 `__bool__` / `__len__` |
| iterator identity | child 的 `__iter__` 必须解析到同一 code object；若可能调用用户自定义 `__iter__`，回退 |
| field layout | `left/right/value` 的 `FieldAccessProof` 必须覆盖 offset、descriptor、type version、split-dict inline-values valid/layout guard 和失效路径 |
| sentinel | 空子树 sentinel 只能来自原始 guard；不得在状态机中新增 `None` 为空的语义 |

`no-match` 只属于 matcher 阶段：任何字段、truthiness、iterator identity 或 layout 证明不足时，pass 在 CFG 改写前返回原 HIR。已经生成状态机后，运行时 guard 失败不得再走 `no-match`，只能执行 `RuntimeFailureAction`。

`RuntimeFailureAction` 按启用层级区分：

| 层级 | 行为 |
| ---- | ---- |
| 实验路径 | 只允许测试/benchmark 专用配置使用 `kExperimentalFailClosed`。该动作必须在任何 current/stack/active-path 状态突变前清理 `TreeIterState`、使当前 optimized entry 失效，并抛出确定性的内部实验 bailout；它不声明 Python 语义等价，不能在生产配置或默认开启路径出现。若无法把失败点放在状态突变前，pass no-op |
| 生产路径 | guard 失败、layout dependency 失效、descriptor/property 变化或 fallback 可观察时，必须 invalidate 或 exact deopt/reify 到原始 generator/yield-from 状态；无法 reify 时 pass no-op |

实验准入和生产准入分开处理。实验准入只证明 allowlisted 目标函数在受控 `next()`/`for`/`list()` 消费下值得继续实现；生产准入面向 M5/default，必须是逐函数、可验证、fail-closed 的五个纵切面 artifact。

实验准入必须包含：

| 项目 | 要求 |
| ---- | ---- |
| code-object allowlist | 目标 `Tree.__iter__` 必须由 TreeIter 专用精确 code-object allowlist、精确 jitlist 条目或专用 harness 触发；wildcard jitlist、低 `PYTHONJITAUTO`、普通 `compile_after_n_calls()` 不能单独放行 |
| HIR 形态证据 | 当前 master HIR 能识别 left/value/right 骨架、原始 child 跳过 guard、truthiness 等价性和 yield-from 链路 |
| 类型与布局证据 | M1.5 只要求实验样例中 owner/child exactness、field layout、iterator identity 有可审查证据；不要求此时已有生产 invalidation verifier |
| 协议边界 | 已优化实验 generator 的 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、suspended deopt 和 instrumentation attach 必须 fail closed，不能依赖“调用方不会这样用”的约定 |

生产准入不是单个 HIR dump 结论，而是逐函数五个纵切面 artifact。每个被优化的 production function 都必须同时给出证明来源、运行时 guard 或失效/deopt 处理：

| 纵切面 | 详细要求 |
| ------ | -------- |
| HIR 结构 | 当前 master HIR 能稳定识别 left/value/right 中序骨架、原始 child 跳过 guard、truthiness 等价性、yield-from `Send`/`YieldValue::yieldFromIter()` 链路和必要 FrameState；HIR 形态变化时 matcher no-match 或重新生成 artifact |
| owner/child exactness | owner type 与非空 child type 必须来自 Static Python sealed 约束、type-version dependency、retained `CheckExact` guard 或等价来源；observed local value 只能作为候选信号 |
| field layout | `left/right/value` offset、descriptor/property 副作用、type version 和 layout dependency 必须有明确 guard 或 invalidation；CFG 改写前证明不足时 no-match，已生成代码的布局变化必须使 optimized code 失效或 exact deopt/reify |
| iterator identity | child `__iter__` 必须解析到同一 code object，且覆盖子类覆盖、descriptor、custom iterator protocol 的失效路径；不能用源码同名推断代替 |
| 失效/deopt | CFG 改写前的证明不足必须 no-match；已生成状态机后的任一 guard/dependency 失败必须有明确 `RuntimeFailureAction`，即实验 fail-closed、invalidate 或 exact deopt/reify；active-path 回边、深度越界和协议敏感操作也必须纳入该纵切面 |

生产 artifact 必须使用版本化 schema，并由 CI 或 release 流程中的 verifier 对当前 master fail-closed 校验：

| Schema 字段 | 要求 |
| ----------- | ---- |
| source identity | CinderX commit、CPython source tag/commit、目标 code object identity/hash、目标函数 qualname |
| HIR fingerprint | HIR dump hash、matched guard blocks、truthiness/None guard 类型、yield-from `Send`/`YieldValue::yieldFromIter()` 链路、必要 FrameState id |
| exactness/layout | owner/child type source、default truthiness proof、type-version dependency、FieldAccessProof kind、field offset、split-dict valid offset、descriptor/property 判定、layout invalidation hook |
| iterator identity | child `__iter__` code object identity、subclass/descriptor/custom iterator negative evidence、失效 hook |
| deopt/reify capability | exact reify 可表示性 artifact id、active-path/depth guard id、协议入口 guard id |
| verifier result | 生成命令、校验时间、输入 HIR/commit、pass/fail 结果；任一字段缺失、hash 不匹配或 verifier 版本过期时 production gate false |

实现前必须提供至少一个当前 master 准入样例，但样例按实验/生产目标分层组织：

| 样例 | 要求 |
| ---- | ---- |
| pyperformance `generators` | M1.5 需证明当前 master HIR 的 truthiness guard、split-dict `FieldAccessProof` 和实验 allowlist 可匹配；M4 以该用例收益达标作为 V2 成功条件；M5/default 还必须提交并验证 production artifact |
| pivot 候选真实 workload | 若 pyperformance 不满足实验准入，可记录非合成、生产等价的 TreeIter workload、owner、代码对象和实验准入证据，但只能作为后续 pivot 决策输入；本 V2 不用它替代 M2-M4 或 M4 成功标准 |
| 本地回归 TreeIter | 只允许作为 correctness、栈扩容、codegen smoke 和 matcher regression 样例；不得替代 M1.5 `generators` gate，也不得用于声明 pyperformance 或生产等价收益 |
| 负例 | 子类覆盖、descriptor/property、副作用 `__iter__`、自定义 truthiness、裸 `yield from None`、split-dict fallback 无法处理均不触发状态机 |

M1.5 `generators` 接受标准必须在进入 M2 前写入 artifact；pivot 候选 workload 可以复用同一 schema 记录，但不能改变 V2 gate：

| 标准 | 要求 |
| ---- | ---- |
| 来源与 owner | 来自 pyperformance、生产等价 benchmark、或明确 owner 维护的真实 workload；记录来源、owner 和更新责任 |
| 数据形态 | 记录树深度、左右 skew、`None` 子树比例、shared subtree 是否出现、样本规模和生成方式 |
| 代码身份 | 记录目标 code object、数据模型、`FieldAccessProof`、字段布局来源和 allowlist/jitlist 精确条目 |
| 执行口径 | 固定命令、环境变量、JIT on/off、feature on/off、平台、CPU 策略和运行次数 |
| 稳定性 | 至少达到 M4 性能统计所需重复次数和方差要求；不稳定时不得作为性能 gate |

### 9.2.3 状态机 CFG 生成算法

状态机阶段：

```cpp
enum class TreeIterPhase : int32_t {
  kLeft = 0,
  kYield = 1,
  kRight = 2,
  kBacktrack = 3,
  kExit = 4,  // production active-path/depth cleanup marker
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
bb_exit
bb_done
```

关键生成步骤：

```text
1. 找到 InitialYield 所在 init_block。
2. 在 InitialYield 前插入：
   EnsureTreeIterState(original_frame_state)
   SaveCurrentNode(self_reg)
   LoadConst(kLeft)
   SavePhase(kLeft)
3. splitAfter(InitialYield)，保留 init_block 作为状态机入口前置块。
4. AllocateBlock 创建状态机块。
5. init_block append Branch(bb_loop)。
6. bb_loop LoadPhase 后做 kLeft/kYield/kRight/kBacktrack/kExit dispatch。
7. left/right 阶段每次 `LoadTreeIterField` 后都必须重放原始 guard 对当前动态 child value 的判定；truthiness guard 必须先通过 default truthiness 证明，不得新增裸 `Py_None` 为空的语义。
8. 对非空 child 必须在任何 current/stack/active-path 状态突变前执行 exact owner type、iterator identity 和字段 proof 的 runtime guard。matcher 的静态证明只能授权生成这些 guard，不能替代动态检查。生产路径在进入 child 前还必须检查 active-path 回边和原始递归深度预算，命中时 exact deopt/reify；首版实验若未实现这些前置 guard 只能保持 no-op 或受控 harness。
9. yield 阶段 LoadCurrentNode、LoadTreeIterField(value_proof)、YieldValue(original_frame_state)；首版实验只声明 `next()`/`for`/`list()` 受控消费结果正确，不保留 yield-from 元数据，也不执行精确 deopt。生产配置在 9.6.6 的协议/deopt/active-path gate 未实现前保持 no-op。
10. backtrack 阶段 LoadStackTop，空栈到 done，非空到 pop。
11. pop 阶段 StateStackPop、LoadPoppedPhase、SaveCurrentNode、SavePhase。
12. done 阶段通过 `ClearTreeIterState` 清理 tree_iter 状态引用，再 Return Py_None。
13. 删除不可达块，重新推导新寄存器类型。
```

初始化插入必须使用 `BasicBlock::insert()`，不得调用会 append 到块末尾的辅助函数创建被 `SavePhase` 使用的常量。否则会出现定义晚于使用的 SSA 违规。

### 9.2.4 状态转移算法

```text
LEFT:
  current = LoadCurrentNode()
  left = LoadTreeIterField(current, left_proof)
  left_guard = CheckTreeIterChildValue(left, left_guard_proof)
  if left_guard == no_child:
      SavePhase(kYield)
  else:
      # 动态 left 已被 CheckTreeIterChildValue 证明为 exact supported Node，
      # iterator identity 与 left_proof 的 guard/runtime_failure_action
      # 可支撑当前启用层级。
      CheckTreeIterChildEntry(left)     # production only; no mutation before this succeeds
      StateStackPush(current, kYield)
      TreeIterEnterChild(left)
      SaveCurrentNode(left)
      SavePhase(kLeft)
  goto loop

YIELD:
  current = LoadCurrentNode()
  value = LoadTreeIterField(current, value_proof)
  YieldValue(value, original_frame_state)
  SavePhase(kRight)
  goto loop

RIGHT:
  current = LoadCurrentNode()
  right = LoadTreeIterField(current, right_proof)
  right_guard = CheckTreeIterChildValue(right, right_guard_proof)
  if right_guard == no_child:
      SavePhase(kBacktrack)
  else:
      # 动态 right 已被 CheckTreeIterChildValue 证明为 exact supported Node，
      # iterator identity 与 right_proof 的 guard/runtime_failure_action
      # 可支撑当前启用层级。
      # production active-path/depth path uses kExit to leave parent after right subtree completes
      CheckTreeIterChildEntry(right)    # production only; no mutation before this succeeds
      StateStackPush(current, kExit)
      TreeIterEnterChild(right)
      SaveCurrentNode(right)
      SavePhase(kLeft)
  goto loop

BACKTRACK:
  if LoadStackTop() == 0:
      TreeIterLeaveCurrentNode()
      ClearTreeIterState()
      return Py_None
  TreeIterLeaveCurrentNode()
  node = StateStackPop()
  phase = LoadPoppedPhase()
  SaveCurrentNode(node)
  SavePhase(phase)
  goto loop

EXIT:
  TreeIterLeaveCurrentNode()
  SavePhase(kBacktrack)
  goto loop
```

注意：进入左子树时 push parent 的 phase 是 `kYield`，不是 `kRight`。左子树结束后需要先 yield parent value，再处理右子树。

`CheckTreeIterChildValue` 是伪操作，不要求一定新增单独 HIR opcode；实现可以用现有 `Is`, `CondBranch`, `CheckExact`, type-version guard 和 iterator identity guard 展开。约束是这些检查必须支配 `StateStackPush`、`TreeIterEnterChild`、`SaveCurrentNode` 和 `SavePhase(kLeft)`，且失败路径只能执行当前启用层级允许的 `RuntimeFailureAction`。

首版实验状态转移算法不生成通用 Python attribute fallback 分支。所有 child exactness、truthiness、field access、layout 和 iterator identity 证明必须在 matcher 阶段完成；split-dict proof 可以生成 valid/layout fast guard，但 guard 失败只能按 `RuntimeFailureAction` fail closed，不能静默继续读取裸 offset。无法在 CFG 改写前证明的函数保持原 HIR 不变。生产路径若实现状态机，进入 child 前必须先执行 active-path 和语义深度预算检查；active-path 回边、深度越界或字段 proof 失效必须 exact deopt/reify 到原始 generator，而不是 silently 遍历或依赖 heap stack 支持更深结构。

生产 active-path/depth 采用身份栈语义：`active_path` 持有当前原始递归调用链中的节点 identity，并持有强引用以避免地址复用；`tree_iter_depth` 等于 active-path 长度；`tree_iter_depth_budget` 来自进入状态机时原始 generator 还能合法递归进入的剩余深度。M1.6 必须用当前 CPython/CinderX recursion-limit 入口确认预算来源；无法确认时 `cycle_depth_semantics_enabled=false`。

| 转移点 | 检查与更新顺序 |
| ------ | -------------- |
| 初始化 self | `EnsureTreeIterState` 后将 `self` 加入 active-path，`depth=1`；失败时不写 current/phase |
| 进入 left child | 先 `CheckTreeIterChildEntry(left)`，检查 identity 未在 active-path 且 `depth + 1 <= depth_budget`；失败时 exact deopt/reify 且不修改 stack/current/active-path；成功后 push parent/kYield、加入 child、`depth++`、再 `SaveCurrentNode(left)` |
| left subtree 完成 | BACKTRACK 先 `TreeIterLeaveCurrentNode()` 移除当前 child 并 `depth--`，再 pop parent/kYield 恢复 parent；parent 在 active-path 中保持不变 |
| 进入 right child | 先 `CheckTreeIterChildEntry(right)`；成功后 push parent/kExit 作为右子树完成后的退出标记，加入 child、`depth++`、再 `SaveCurrentNode(right)`；parent 仍在 active-path 中，模拟原始 generator 正在 delegated yield-from right |
| right subtree 完成 | BACKTRACK 移除当前 right path 节点后 pop parent/kExit；kExit 阶段再 `TreeIterLeaveCurrentNode()` 移除 parent 并进入 BACKTRACK |
| shared subtree | 只要节点已从 active-path 移除，再次进入不视为 cycle；若仍在 active-path 中则必须在任何状态突变前 exact deopt/reify |

### 9.2.5 codegen 算法

状态机 LIR opcode 的常规热路径不走 C runtime helper；`EnsureTreeIterState` 初始化和 heap stack 扩容失败/慢路径可以调用 helper，并必须保留异常契约。首版通过 frame pointer 正偏移读取 `GenDataFooter.tree_iter_state` 指针，再访问 heap state 字段：

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
EnsureTreeIterState:
  if footer.tree_iter_state == nullptr:
      state = allocateTreeIterState()
      if state == nullptr:
          raise MemoryError through original FrameState
      footer.tree_iter_state = state

LoadCurrentNode:
  state = footer.tree_iter_state
  output = state.current_node
  if output != nullptr:
      Py_INCREF(output)

SaveCurrentNode:
  state = footer.tree_iter_state
  assert state != nullptr
  new = input
  if new != nullptr:
      Py_INCREF(new)
  old = state.current_node
  state.current_node = new
  if old != nullptr:
      Py_DECREF(old)
```

`SaveCurrentNode` 必须先持有新输入再释放旧 current。`LoadField(left/right)` 在当前 HIR 中可能产生 borrowed child；若先 `DECREF(old)`，旧节点析构可能释放或修改 child，使后续 `INCREF(input)` 触碰悬挂指针。`old == input` 时采用先 `INCREF` 后 `DECREF` 的顺序，净引用计数不变且不会暴露 borrowed ref 窗口。

字段读取：

```text
LoadTreeIterField(node, slot_or_member_proof):
  assert node exact owner type or guard/dependency active
  output = *(PyObject**)(node + proof.value_offset)
  if output != nullptr:
      Py_INCREF(output)

LoadTreeIterField(node, split_dict_proof):
  if !checkInlineValuesValid(node, proof.valid_offset, proof.layout_dependency):
      handle proof.runtime_failure_action   # experimental fail-closed/invalidate/exact deopt; no silent fallback
  output = *(PyObject**)(node + proof.value_offset)
  if output != nullptr:
      Py_INCREF(output)
```

split-dict 版本不得在状态机热路径直接调用 `LoadAttr` fallback。fallback 形态由 `FieldAccessProof` 记录，用于证明原始 HIR 语义和生产 exact deopt/reify 的目标状态；若当前启用层级没有处理 fallback 的能力，pass 必须 no-op 或由受控实验 harness 证明该 fallback 不会发生。

栈操作：

```text
StateStackPush(node, phase):
  state = footer.tree_iter_state
  assert state != nullptr
  top = state.stack_top
  if top >= state.stack_capacity:
      if !growTreeIterStateStack(state):
          raise MemoryError on controlled slow path
  if node != nullptr:
      Py_INCREF(node)
  state.stack[top].node = node
  state.stack[top].phase = phase
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

ClearTreeIterState:
  state = footer.tree_iter_state
  if state != nullptr:
      clearTreeIterState(footer)
```

`StateStackPop` 不对 `output` 额外 `INCREF`，因为栈持有的引用转移给返回寄存器，后续 `RefcountInsertion` 的 `XDecref` 负责释放。

`safe_overflow_path` 是 release 必需路径，不是 debug 断言。V1.4 固定选择可增长 heap stack + 原始递归语义深度预算：

| 项目 | 要求 |
| ---- | ---- |
| 初始容量 | 16 个 `TreeIterStackEntry`，覆盖旧分支主要验证深度 |
| 扩容触发 | 每次 push 前比较 `stack_top` 和 `stack_capacity`，且确认继续进入 child 不超过原始递归 generator 可接受的语义深度 |
| 扩容方式 | 调用 runtime/helper 重新分配 stack buffer，复制旧条目，保持每个 owned `PyObject*` 引用不变 |
| 扩容失败 | 设置 `MemoryError` 或等价受控异常，不能写越界，不能泄漏已 owned 引用 |
| 深度越界 | 若继续进入 child 会超过原始递归语义边界，生产路径 exact deopt/reify；无法 reify 时该函数生产配置 no-op |
| active-path 回边 | 进入 `left/right` child 前检查 child 是否已在当前递归路径中；命中时 production exact deopt/reify；离开路径后允许 shared subtree 再次进入 |
| GC/clear | `visitTreeIterState` 与 `clearTreeIterState` 按 `[0, stack_top)` 遍历动态 stack |

不得在 release 构建中只依赖 `JIT_DCHECK`，也不得把 heap stack 动态扩容解释为对更深递归树的隐式语义增强。若 active-path/depth guard 未闭合，生产配置必须 no-op；实验 harness 可以继续覆盖有限无环树和扩容安全。

状态 op 的异常和提交顺序契约：

| 指令 | 是否可 raise | FrameState | 提交顺序与所有权 |
| ---- | ------------ | ---------- | ---------------- |
| `EnsureTreeIterState` | 是，分配失败时 `MemoryError` | 使用原始 `YieldValue` 或最近 dominating `FrameState`，缺失则 matcher no-match | 只有分配并初始化成功后才写 `footer.tree_iter_state`；失败时 footer 保持原值，不持有新引用 |
| `SaveCurrentNode` | 不主动分配，但 `Py_DECREF(old)` 可执行析构 | 不产生 Python 异常恢复边；必须作为 clobber/decref 屏障 | 先 `Py_INCREF(new)`，再写 `current_node`，最后 `Py_DECREF(old)`；`old == new` 安全 |
| `StateStackPush` | 是，heap stack 扩容失败时 `MemoryError`；生产路径 active-path/depth guard 可触发 exact deopt/reify | 使用原始 `YieldValue` 或最近 dominating `FrameState`，缺失则 matcher no-match；生产 guard 还需要 reify contract | active-path/depth 检查和扩容成功后才 `Py_INCREF(node)` 并写槽位；失败或 deopt 时不修改 `stack_top`、不写槽位、不新增 owned ref |
| `StateStackPop` | 否；空栈只能由 CFG 中 `LoadStackTop() == 0` 分支避免 | 不需要异常 FrameState；debug 下可断言 | 先递减 `stack_top`，再把槽位 owned node 转移到输出寄存器，清空槽位避免二次释放 |
| `CheckTreeIterChildEntry` | 生产路径可触发 exact deopt/reify | 使用当前 phase 对应的 reify contract；缺失则 production gate no-op | 只读 active-path/depth，不修改 current/stack/active-path；失败时不产生部分状态 |
| `TreeIterEnterChild` | 不主动 raise，但会持有 child identity 引用 | 不产生 Python 异常恢复边；必须作为 refcount 屏障 | 在 `CheckTreeIterChildEntry` 成功后执行，向 active-path 加入 child 并 `depth++`；失败时不得写 current/stack |
| `TreeIterLeaveCurrentNode` | 不传播新异常，但 decref 可执行 finalizer side effect | 不需要异常 FrameState；必须可重复防御清理 | 如果 current identity 仍在 active-path 中，则移除 current、`depth--` 并释放 active-path owned ref；如果已由 kExit 等路径移除则 no-op；不得移除仍代表 suspended parent 的 kExit marker |
| `ClearTreeIterState` | 不传播新异常，但可执行 decref/finalizer side effect | 不需要异常 FrameState；必须在 clear/dealloc backstop 可重复调用 | 清理 current 和 `[0, stack_top)` owned refs，置空槽位并归零 `stack_top`；重复调用幂等 |

能分配或扩容的 HIR 必须在 IR 中显式可见：首版采用 `EnsureTreeIterState` 和 `StateStackPush` 携带/绑定 `FrameState` 的形式，或在 lowering 前拆成 status-returning helper + `CheckNeg` / `CheckExc`。不得把分配失败隐藏在无异常边的普通 LIR side effect 中。

## 9.3 行为模型

### 9.3.1 正常流程

```text
1. 用户代码创建满足准入条件的 Tree/Node 对象，调用 list(tree) 或 for 循环。
2. JIT 编译 Node.__iter__。
3. TreeIterStateMachinePass 在 HIR 优化阶段识别目标模式、原始 child guard、truthiness 等价性、`FieldAccessProof`、实验准入证据、生产五纵切面 artifact verifier 和实验/生产 gate 条件。
4. pass 生成状态机 CFG，并移除原始递归 yield-from 路径。
5. 生成器首次执行 InitialYield 前保存 self 和初始 phase。
6. 每次 resume 后进入 bb_loop，根据 current_phase 分派。
7. 状态机通过 `GenDataFooter.tree_iter_state` 指向的 heap state 持久化 current_node、phase、stack；生产路径还维护 active-path 和原始递归深度预算。
8. YIELD 阶段暂停 generator；首版实验只声明 `next()`/`for`/`list()` 消费结果正确，生产路径在协议敏感操作、active-path 回边或深度越界无法 exact deopt/reify 时不触发优化。
9. BACKTRACK 阶段栈为空时返回 None，generator 结束。
```

### 9.3.2 异常流程

| 场景 | 处理 |
| ---- | ---- |
| HIR 模式不匹配 | pass 直接 return，保留原始 HIR |
| 缺少 `left/right/value` 任一字段访问证明 | pass 直接 return |
| 找不到 `InitialYield`、`self_reg` 或普通 `YieldValue` FrameState | pass 直接 return |
| 缺少原始 child 跳过 guard 且 child 可能为 None | pass 直接 return，保留 CPython TypeError 语义 |
| truthiness guard 无法证明 default truthiness | pass 直接 return，保留用户 `__bool__` / `__len__` 语义 |
| split-dict proof 的 valid/layout guard 或 fallback 处理缺失 | pass 直接 return；生产配置必须具备 invalidate 或 exact deopt/reify 能力 |
| child 不是同一 exact Node 类型或 `__iter__` 可能被覆盖 | pass 直接 return |
| `gi_yieldfrom`、`send/throw/close`、StopIteration value 无法 exact deopt/reify | 生产配置 pass 直接 return；实验配置只能覆盖已声明的 `next()`/`for`/`list()` 受控场景 |
| 生成状态机中途失败 | 不允许留下半改写 CFG；实现应先完成匹配和字段收集，再一次性改写 |
| `state_stack` 容量不足 | release 必须在写入前、且仍在语义深度预算内进入 heap stack grow 路径；扩容失败走受控错误路径 |
| active-path 回边或原始递归深度越界 | 生产配置必须 exact deopt/reify 到原始 generator/yield-from 状态；无法 reify 时 pass 直接 return |
| 对象 refcount 降为 0 | `SaveCurrentNode` 或 codegen decref 路径必须调用 `_Py_Dealloc`，不得只写回负 refcount |
| deopt 请求 | 生产配置必须满足 9.6.4 的 deopt contract；首版实验不声明 production deopt，生产 gate 未闭合时禁用优化 |
| 目标架构无 codegen | 编译期 `CINDER_UNSUPPORTED` 或配置关闭 |

## 9.4 数据模型

### 9.4.1 数据结构定义

#### 9.4.1.1 `TreeIterMatch`

新增内部结构，建议定义在 `tree_iter_state_machine_pass.h`。`FieldAccessProof` 与 `ChildGuardProof` 是 matcher 结果的一部分：

```cpp
enum class FieldAccessKind {
  kSlotOrMember,
  kSplitDict,
};

enum class ChildGuardKind {
  kNoneGuard,
  kDefaultTruthinessGuard,
};

enum class MatchRejectReason {
  kUnsupportedShape,
  kMissingGuard,
  kMissingFieldProof,
  kMissingIteratorIdentity,
  kMissingRuntimeFailureAction,
};

enum class RuntimeFailureAction {
  kExperimentalFailClosed,
  kInvalidate,
  kExactDeoptReify,
};

struct FieldAccessProof {
  FieldAccessKind kind;
  BorrowedRef<PyTypeObject> owner_type;
  std::string field_name;
  intptr_t value_offset{0};
  std::optional<intptr_t> valid_offset;
  GuardSource guard_source;
  DependencySource layout_dependency;
  FallbackShape fallback_shape;
  RuntimeFailureAction runtime_failure_action{
      RuntimeFailureAction::kExperimentalFailClosed};
};

struct ChildGuardProof {
  ChildGuardKind kind;
  Instr* guard_instr{nullptr};
  bool requires_default_truthiness{false};
};
```

`kExperimentalFailClosed` 的默认初值只表示默认关闭实验路径的保守占位。生产 matcher 必须显式选择 `kInvalidate` 或 `kExactDeoptReify`；若无法选择，返回 `MatchRejectReason::kMissingRuntimeFailureAction` 并保持原 HIR。

```cpp
struct TreeIterMatch {
  Register* self_reg{nullptr};
  Instr* initial_yield{nullptr};
  FrameState* yield_frame_state{nullptr};
  Type exact_node_type{TTop};

  FieldAccessProof left_field;
  FieldAccessProof right_field;
  FieldAccessProof value_field;

  ChildGuardProof left_guard;
  ChildGuardProof right_guard;
  const YieldValue* left_yield_from{nullptr};
  const YieldValue* value_yield{nullptr};
  const YieldValue* right_yield_from{nullptr};

  bool production_admission_manifest_verified{false};
  bool can_exact_reify_yield_from_protocol{false};
  bool can_deopt_state_machine{false};
  bool can_enforce_active_path{false};
  bool can_enforce_depth_budget{false};
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
  kExit = 4,
};
```

#### 9.4.1.3 `GenDataFooter` 与 `TreeIterState`

`GenDataFooter` 属于所有 JIT generator 的 suspend data。首版不得把固定 16 项 `state_stack` 直接内嵌进去，否则每个非 TreeIter JIT generator 都会增加 256B+ 内存面。V1.4 固定使用最小 footer 指针 + 按需 heap state；生产路径在 heap state 中补充 active-path 和语义深度预算：

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

struct TreeIterActivePath;

struct TreeIterState {
  PyObject* tree_iter_current_node{nullptr};
  int32_t tree_iter_current_phase{0};
  int32_t tree_iter_stack_top{0};
  int32_t tree_iter_stack_capacity{0};
  int32_t tree_iter_depth{0};
  int32_t tree_iter_depth_budget{0};
  int32_t tree_iter_popped_phase{0};
  int32_t tree_iter_reserved{0};

  TreeIterStackEntry* tree_iter_stack{nullptr};
  TreeIterActivePath* tree_iter_active_path{nullptr};
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
| TreeIter 生产路径 | 首次进入状态机前还初始化 active-path 集合和原始递归深度预算；未闭合时生产 no-op |
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
  -> EnsureTreeIterState(GenDataFooter, FrameState)
  -> SaveCurrentNode(self)
  -> GenDataFooter.tree_iter_state->tree_iter_current_node
  -> LoadCurrentNode()
  -> LoadTreeIterField(left/right/value FieldAccessProof)
  -> production guard: CheckTreeIterChildEntry(child) before child entry
  -> production state: TreeIterEnterChild(child) after guard succeeds
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

GC traverse 需要经 `footer->tree_iter_state` 访问 `tree_iter_current_node`、`tree_iter_stack[i].node` 和生产 active-path 中仍持有的节点引用；clear/dealloc 需要 `Py_CLEAR` 所有这些字段并释放 heap stack 与 active-path 辅助结构。

## 9.5 接口设计

### 9.5.1 内部接口设计

```text
TreeIterStateMachinePass::Run(Function&)
  -> matchTreeIter(Function&) -> optional<TreeIterMatch>
  -> extractFieldAccessProof(Function&, Register*, name) -> optional<FieldAccessProof>
  -> proveChildGuard(Function&, Register*, guard) -> optional<ChildGuardProof>
  -> buildTreeIterStateMachine(Function&, TreeIterMatch&)
```

状态机 builder 不暴露给其它 pass。`FieldAccessProof` 首版可定义在本 pass 中，但字段证明语义应保持可提升为共享 helper。所有状态访问通过新增 HIR 指令表达，不在 pass 中直接生成 CallCFunc。

### 9.5.2 内部接口定义

#### 9.5.2.1 Pass 接口

```cpp
class TreeIterStateMachinePass : public Pass {
 public:
  TreeIterStateMachinePass() : Pass("TreeIterStateMachinePass") {}
  void Run(Function& func) override;

 private:
  std::optional<TreeIterMatch> matchTreeIter(Function& func) const;
  std::optional<FieldAccessProof> extractFieldAccessProof(
      Function& func,
      Register* base,
      std::string_view field_name) const;
  std::optional<ChildGuardProof> proveChildGuard(
      Function& func,
      Register* child,
      Instr* guard) const;
  void buildTreeIterStateMachine(Function& func, const TreeIterMatch& match);
};
```

#### 9.5.2.2 HIR 指令接口

除 `LoadTreeIterField` 外，状态读写指令可以用简单 HIR opcode 表达：

```cpp
DEFINE_SIMPLE_INSTR(EnsureTreeIterState, (), Operands<0>, DeoptBase);
DEFINE_SIMPLE_INSTR(SaveCurrentNode, (TObject), Operands<1>);
DEFINE_SIMPLE_INSTR(LoadCurrentNode, (TObject), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(SavePhase, (TCInt32), Operands<1>);
DEFINE_SIMPLE_INSTR(LoadPhase, (TCInt32), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(StateStackPush, (TObject, TCInt32), Operands<2>, DeoptBase);
DEFINE_SIMPLE_INSTR(StateStackPop, (TObject), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(LoadPoppedPhase, (TCInt32), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(LoadStackTop, (TCInt32), HasOutput, Operands<0>);
DEFINE_SIMPLE_INSTR(CheckTreeIterChildEntry, (TObject), Operands<1>, DeoptBase);
DEFINE_SIMPLE_INSTR(TreeIterEnterChild, (TObject), Operands<1>);
DEFINE_SIMPLE_INSTR(TreeIterLeaveCurrentNode, (), Operands<0>);
DEFINE_SIMPLE_INSTR(ClearTreeIterState, (), Operands<0>);
```

`LoadTreeIterField` 不能只靠 `Operands<1>` 隐式读取裸 offset。它必须携带不可变 proof payload 或 verifier 分配的 proof id，并在 HIR printer/parser、copy/clone、CSE/DCE、LIR lowering 和 codegen 中保持一致：

```cpp
using TreeIterFieldProofId = uint32_t;

class LoadTreeIterField final : public Instr {
 public:
  LoadTreeIterField(
      Register* node,
      TreeIterFieldProofId proof_id,
      const FieldAccessProof& proof,
      FrameState* frame_state);

  TreeIterFieldProofId proofId() const;
  const FieldAccessProof& proof() const;
};
```

实现可以选择只在 HIR 中保存 `proof_id`，由 `Function` 或 pass-owned immutable table 保存 `FieldAccessProof`；也可以把小型 proof payload 存入指令。两种方案都必须满足：

| 契约 | 要求 |
| ---- | ---- |
| 打印/解析 | HIR dump 至少打印 kind、field name、owner type、value offset、valid offset、runtime failure action 和 proof id；测试覆盖 round-trip 或 verifier table 重新绑定 |
| 优化 pass | cloning、block split、DCE、CSE 不能丢失 proof id；不同 proof id 的 `LoadTreeIterField` 不得只因 operand 相同被合并 |
| LIR lowering | lowering 消费 proof，生成 slot/member 或 split-dict 的 immediates、valid guard、deopt/invalidate patchpoint 和实验 fail-closed helper 调用 |
| deopt metadata | 生产路径的 `kExactDeoptReify` 必须能从 proof id 找回 fallback shape、FrameState 和 reify contract；缺失时 production gate false |

输出类型：

| 指令 | 输出类型 |
| ---- | -------- |
| `LoadCurrentNode` | `TObject` |
| `StateStackPop` | `TObject` |
| `LoadTreeIterField` | `TObject` |
| `LoadPhase` | `TCInt32` |
| `LoadPoppedPhase` | `TCInt32` |
| `LoadStackTop` | `TCInt32` |

#### 9.5.2.3 LIR 接口

新增 LIR opcode：

```cpp
kEnsureTreeIterState
kSaveCurrentNode
kLoadCurrentNode
kSavePhase
kLoadPhase
kStateStackPush
kStateStackPop
kLoadPoppedPhase
kLoadStackTop
kLoadTreeIterField
kCheckTreeIterChildEntry
kTreeIterEnterChild
kTreeIterLeaveCurrentNode
kClearTreeIterState
```

HIR lowering 必须使用 `appendInstr(Instruction::kXxx)` 或带 output 的 `appendInstr(output, Instruction::kXxx)`，不得在状态机 pass 中直接生成 `CallCFunc`。旧分支已验证 `appendCallInstruction` 生成的是通用 `kCall`，不会触发 `BEGIN_RULES(Instruction::kXxx)` 的原生 translate 规则。`kLoadTreeIterField` 的 LIR instruction 必须携带 proof id 或 lowering 后的 proof immediate bundle；regalloc、postalloc 和 codegen 不能把它降级成普通 base+offset load。`kEnsureTreeIterState` 可调用分配 helper，但必须保留异常边和 `FrameState`；`kClearTreeIterState` 可在 lowering/codegen 中调用统一的 `clearTreeIterState(GenDataFooter*)` helper，因为它只出现在 done、frame clear 和 dealloc backstop 路径，不属于热路径状态读写。

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

实验配置和生产配置使用不同 gate。实验 gate 是 executable runtime/pass gate，只限制是否生成实验状态机 HIR。生产 gate 由两类输入组成：pass-time admission verifier 结果和 runtime capability bits。admission artifact 不是用户可设置的 runtime boolean，而是版本化 manifest/verifier 对当前 code object、HIR fingerprint 和五个纵切面的 fail-closed 校验结果；校验缺失、过期或 hash 不匹配时，pass 视为 false 并保持 no-op。

```text
experimental_enabled =
  tree_iter_state_machine
  && target_arch_verified
  && experimental_exact_code_object_allowlisted
  && heap_tree_iter_state_enabled
  && field_access_proof_enabled
  && experimental_tree_iter_core_enabled
  && experimental_protocol_fail_closed_enabled
```

生产配置还必须满足：

```text
production_enabled =
  tree_iter_state_machine
  && target_arch_verified
  && heap_tree_iter_state_enabled
  && production_admission_manifest_verified
  && generator_protocol_model_enabled
  && deopt_reify_model_enabled
  && cycle_depth_semantics_enabled
```

gate 来源必须显式落到代码、配置或 CI 证据，不能只因为 `PYTHONJITTREEITERSTATEMACHINE=1` 就生成状态机 HIR：

| Gate | 执行来源 |
| ---- | -------- |
| `tree_iter_state_machine` | `cinderx/Jit/config.h` / `pyjit.cpp` 中的独立配置位和环境变量，默认 `false` |
| `target_arch_verified` | pass 或 config 的架构 allowlist；未在测试矩阵中验证的平台返回 no-op，并由 `test_tree_iter_state_machine_arch_gate` 覆盖 |
| `experimental_exact_code_object_allowlisted` | TreeIter 专用精确 code-object allowlist/jitlist 或专用 harness provenance；wildcard jitlist、低 `PYTHONJITAUTO` 和普通全局 `compile_after_n_calls()` 不设置该位 |
| `production_admission_manifest_verified` | pass 在编译目标函数时读取版本化 admission manifest，并由 verifier 对 HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt 五个纵切面均返回 pass；pyperformance 覆盖声明还必须由 `test_tree_iter_state_machine_admission_artifact` 或等价 CI artifact 支撑。该位不可由环境变量或普通配置直接打开 |
| `heap_tree_iter_state_enabled` | `GenDataFooter.tree_iter_state`、`EnsureTreeIterState`、`clearTreeIterState`、`visitTreeIterState` helper 均已接入并通过 lifecycle/GC/分配失败测试；未完成 M2 前 pass 不插入 |
| `experimental_tree_iter_core_enabled` | 显式实验配置和实验 matcher 通过；不得由低 `PYTHONJITAUTO` 阈值隐式扩大覆盖面 |
| `field_access_proof_enabled` | M1/M3 必须证明 slot/member 或 split-dict `FieldAccessProof` 的 lowering 和 runtime failure action 可支撑当前启用层级；未完成时 matcher no-op。若作为 runtime gate 实现，应并入 `experimental_tree_iter_core_enabled` 或生产 artifact verifier，不单独暴露给用户 |
| `experimental_protocol_fail_closed_enabled` | 已优化实验 generator 在 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、suspended deopt 和 instrumentation attach 路径上 fail closed；未实现时实验 pass no-op |
| `generator_protocol_model_enabled` | 默认硬编码为 false；只有协议敏感操作前 exact deopt/reify 实现并通过 protocol 矩阵后才能打开 |
| `deopt_reify_model_enabled` | 默认硬编码为 false；只有 `TreeIterState` 到解释器 generator/yield-from 栈 reify contract 实现并通过 deopt 矩阵后才能打开 |
| `cycle_depth_semantics_enabled` | 默认硬编码为 false；只有 active-path 回边检测、shared subtree 允许再次进入、原始递归深度预算和对应 exact deopt/reify 全部通过测试后才能打开 |

实验配置任一 `experimental_enabled` 条件不满足时，`TreeIterStateMachinePass` 必须 no-op，不能生成状态机 HIR。生产配置任一 `production_enabled` 条件不满足时必须 no-op；x86_64 未完成同等 correctness、protocol、deopt 和性能矩阵前，即使配置为 true 也必须禁用生产路径。

`experimental_protocol_fail_closed_enabled` 必须对应可执行 helper，而不是文档承诺。建议实现为 `treeIterExperimentalAbort(gen, reason)`：在 `gi_yieldfrom`、`send(non-None)`、`throw`、显式 `close`、suspended deopt 和 instrumentation attach 入口检测到 active experimental TreeIterState 时，先清理 owned TreeIterState、使当前 optimized entry 失效，再抛出确定性的内部实验 bailout；finalize/tp_clear 路径不能抛异常，只执行清理和失效。该 helper 的行为只用于测试/benchmark 专用实验配置，不声明 CPython 协议等价，生产配置必须依赖 exact deopt/reify 或 no-op。

以下证据不作为用户可设置 runtime boolean，而作为 CI/release checklist：

| Release gate | 证据 |
| ------------ | ---- |
| lifecycle matrix | debug/release、GC、tp_clear、finalize、free-list 复用、正常完成和异常退出测试 artifact |
| performance matrix | 指定 benchmark 命令、平台、重复次数、统计方法和阈值的 CI 或 release artifact |
| production rollout | 上述 artifact、`production_admission_manifest_verified` 与 `production_enabled` 所需 runtime 能力同时满足后，才能评估默认开启或更宽 allowlist |

## 9.6 代码实现要点

### 9.6.1 pass 插入点

当前 pipeline 在 simplify、PrimitiveUnboxCSE、PrimitiveBoxRemat、CleanCFG、DCE 后运行 `RefcountInsertion`。状态机 pass 应插入到 `RefcountInsertion` 前，建议位置：

```text
runPassIf(CleanCFG)
runPassIf(DeadCodeElimination)
runPassIf(CleanCFG)
runPassIf(TreeIterStateMachinePass)  # 若发生改写，pass 内部完成 cleanup 和 reflowTypes
runPass(RefcountInsertion)
```

原因：

1. 状态机新指令需要让 `RefcountInsertion` 处理输出 object 的 `XDecref`。
2. pass 生成 CFG 后需要 cleanup 和 type reflow，避免新寄存器保持 `TTop`；当前代码库提供 `hir::reflowTypes(Function&)` helper 而不是独立 pass，因此 `TreeIterStateMachinePass::Run` 在实际改写后必须先执行 `CleanCFG` / `DeadCodeElimination` / `CleanCFG` 和 `reflowTypes(func)`，再返回给 `RefcountInsertion`。
3. 如果状态机 pass 放在 `RefcountInsertion` 后，新增 object 输出不会获得统一引用计数处理。

### 9.6.2 `InitialYield` 前插入

实现必须显式定位 `InitialYield`：

```cpp
auto* init_block = initial_yield->block();
auto init_iter = iterator_pointing_to(initial_yield);

init_block->insert(EnsureTreeIterState::create(*original_frame_state), init_iter);
init_block->insert(SaveCurrentNode::create(self_reg), init_iter);
init_block->insert(LoadConst::create(init_phase, Type::fromCInt(kLeft, TCInt32)), init_iter);
init_block->insert(SavePhase::create(init_phase), init_iter);
```

`EnsureTreeIterState` 必须携带原始 `FrameState`；缺少可复用 `FrameState` 时 matcher no-match。`SaveCurrentNode` 不再隐藏状态分配，只负责引用所有权安全的 current 更新。不要使用会 append 的 `CreatePhaseConst()` 来创建 `init_phase`。旧分支记录过该错误会导致 `SavePhase(init_phase)` 在定义前执行。

### 9.6.3 child guard、truthiness 和 `nullptr` 检查

状态机不得把裸 `yield from None` 改写为空遍历。空子树分支只能来自原始 HIR 已存在且可证明语义的 child guard：

```text
if original guard proves child is None:
    no_child
else if original guard is truthiness and default truthiness proof says child false iff child is None:
    replay original truthiness branch as None-equivalent child guard
else if child is nullptr from Static Python/internal layout and original semantics treats it as absent:
    no_child
else if matcher has proven child exact type/layout and iterator identity:
    has_child
else:
    no match before CFG rewrite
```

普通 Python 对象字段中的 `Py_None` 只有在原始源码/HIR 已显式跳过 `None`，或 truthiness guard 已证明等价于 `child is not None` 时才能走 `no_child`。`nullptr` 仅用于 Static Python 或内部布局确有 nullable field 语义的场景，必须由类型系统和原始控制流共同证明。若 exact Node 类型定义了 `__bool__`、`__len__`，或 matcher 无法排除这类自定义 truthiness，则 truthiness guard 不可用于 TreeIter 状态机。

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
  clear and free state.tree_iter_active_path
  free state.tree_iter_stack
  free state
  footer.tree_iter_state = nullptr

visitTreeIterState:
  state = footer.tree_iter_state
  if state == nullptr: return
  Py_VISIT(state.tree_iter_current_node)
  for i in [0, state.stack_top):
      Py_VISIT(state.tree_iter_stack[i].node)
  visit state.tree_iter_active_path entries if production path owns references
```

调用点：

| 调用点 | 要求 |
| ------ | ---- |
| `JITRT_AllocateAndLinkGenAndInterpreterFrame` | 初始化 `tree_iter_state` 指针为空 |
| `jitgen_traverse` | visit 当前节点和栈节点 |
| `deopt_jit_gen` | 首版实验路径遇到 active TreeIter state 且无精确 reify 时不得声明成功生产 deopt；生产配置必须 no-op |
| `gen_dealloc_with_custom_free` 或其前置路径 | free-list 复用前释放引用并清零 |
| generator 正常结束路径 | 状态机 done 前或 frame clear 时释放 current/stack 引用 |

V1.4 固定 deopt 决策：首版实验实现不提供 production deopt contract；生产 gate 在 exact reify 未实现前禁用状态机。生产化必须实现 exact reify，不能再以“不可 deopt 状态机”作为普通动态 generator 的隐含假设。

exact reify 设计必须在 contract 表中逐项定义引用转移顺序：

| 路径 | 必须说明 |
| ---- | -------- |
| 正常完成 | done block 何时 `clearTreeIterState`，以及与 `jitFrameClearExceptCode` 的先后 |
| `gi_yieldfrom` | 在可观察 `gi_yieldfrom` 前 reify 等价 delegated yield-from 栈并返回 CPython 原路径可见对象 |
| `send(non-None)` | 在发送前 reify 当前 delegated iterator 状态，并由 CPython 原 yield-from 委派逻辑处理 |
| `close()` | 先 exact deopt/reify 并委派给 CPython 原路径注入 `GeneratorExit` |
| `throw()` | 先 exact deopt/reify 并按 `_gen_throw` 语义传播 |
| active-path 回边 | 在写入 current/stack/active-path 新状态前 reify 到原始递归 generator，让原路径产生对应递归/异常行为 |
| 原始递归深度越界 | 在继续进入 child 前 reify 到原始递归 generator，不通过 heap stack 产生更深遍历语义 |
| instrumentation attach | `jitgen_am_send_with_deopt` / 全量 suspended generator deopt 如何处理状态机 footer |
| tp_clear/finalize/dealloc | GC 清理时哪些字段仍 owned，哪些已转移给解释器 |

M1.6 必须先提交 reify 可表示性 artifact，作为 M2 状态模型冻结前置输入。当前 CinderX generator deopt 依赖 `GenYieldPoint`、`DeoptMetadata`、`reifyGeneratorFrame()` 和 `gi_frame_state`；TreeIter 状态机把递归 yield-from frame 压平成 `TreeIterState` 后，不能只写“后续 reify”而不证明这些信息可恢复。

| Reify artifact 项 | 要求 |
| ----------------- | ---- |
| frame chain mapping | 对每个 `TreeIterPhase`、`state_stack` entry 和 `kExit` marker，列出需要合成的原始 generator/yield-from frame 链 |
| resume location | 每一层 frame 对应 code object、bytecode/resume offset、HIR `FrameState` 或 `DeoptMetadata` 来源 |
| visible state | `gi_yieldfrom` 可见对象、`gi_frame_state`、locals、eval stack、StopIteration value、异常上下文 |
| ownership transfer | `TreeIterState.current_node`、stack node、active-path refs 如何转移到 reified frames，失败时如何回滚 |
| current-master dependency | 记录 CinderX commit、CPython source tag/commit、`GenYieldPoint`/`DeoptMetadata` 布局和 verifier 版本 |
| go/no-go | 若任一 phase/stack 形态无法在当前 CPython/CinderX frame layout 上 executable reify，`deopt_reify_model_enabled=false`，且生产配置 hard no-op |

### 9.6.5 `instr_effects` 策略

首版保守设置：

| 指令 | memoryEffects | hasArbitraryExecution |
| ---- | ------------- | --------------------- |
| `EnsureTreeIterState` | `AManagedHeapAny` | true |
| `SaveCurrentNode` | `AManagedHeapAny` | true |
| `LoadCurrentNode` | `AEmpty` 或 `AOther` | true，因为包含 INCREF |
| `SavePhase` | `AOther` | true |
| `LoadPhase` | `AEmpty` | false |
| `StateStackPush` | `AManagedHeapAny` | true |
| `StateStackPop` | `AOther` | true |
| `LoadPoppedPhase` | `AEmpty` | false |
| `LoadStackTop` | `AEmpty` | false |
| `LoadTreeIterField` | `AManagedHeapAny` 或 guard 读集 | true，split-dict guard 失败可能 invalidate/deopt，且输出需要 INCREF |
| `CheckTreeIterChildEntry` | `AManagedHeapAny` | true |
| `TreeIterEnterChild` | `AManagedHeapAny` | true |
| `TreeIterLeaveCurrentNode` | `AManagedHeapAny` | true |
| `ClearTreeIterState` | `AManagedHeapAny` | true |

旧分支证明不能把 `SavePhase` 标成无副作用，否则优化器可能消除关键状态转换，导致只遍历左叶节点。

### 9.6.6 generator 协议模型

CPython yield-from 的可观察行为不等同于只产出相同值序列。当前 CinderX runtime 通过 `YieldValue::isYieldFrom()` 生成 `StoreGenYieldFromPoint`，并在 suspend 后设置 `FRAME_SUSPENDED_YIELD_FROM`，供 `gi_yieldfrom`、`close()` 和 `throw()` 路径使用。状态机若改用普通 `YieldValue`，必须补足等价模型。

V1.4 固定协议决策为“实验限制 + 生产 exact deopt/reify”，不再把三种互斥方案留给实现阶段：

| 模型 | 首版状态 | 要求 |
| ---- | -------- | ---- |
| 实验限制 | 采用 | 默认关闭；只声明 `next()`/`for`/`list()` 受控消费结果正确；不声明 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、StopIteration value 委派语义生产完备 |
| 保留 delegated iterator 元数据 | 不采用为当前生产方案 | 只能作为后续增强讨论；当前生产方案不依赖显式栈模拟完整 delegated iterator 元数据 |
| exact deopt/reify 后委派 | 生产采用，但非首版实验 | 必须定义 `TreeIterState` 到等价解释器 generator/yield-from 栈的 reify 格式，在协议敏感操作前调用 CPython 原路径 |

生产配置在 exact deopt/reify 实现前必须 no-op。实验测试不得用 “`list(tree)` 正确” 推导 production-ready，只能记录实验性能和受控 correctness。

实验路径还必须有技术边界，不能仅靠调用方约定“只用 `next()` / `for` / `list()`”。`experimental_tree_iter_core_enabled` 需要同时满足显式环境变量和 TreeIter 专用 exact code-object allowlist provenance：测试/benchmark 通过 `PYTHONJITLISTFILE` 精确列出目标 `Tree.__iter__` 并被 TreeIter allowlist verifier 确认为 exact match，或在专用 harness 内设置等价的 TreeIter-specific compile token。普通全局 `cinderx.jit.compile_after_n_calls()`、低 `PYTHONJITAUTO` 阈值和 wildcard jitlist 只能触发普通 JIT 编译，不能设置 `experimental_exact_code_object_allowlisted`。

allowlist 只能限定哪个 code object 被优化，不能限定优化后的 generator object 如何被消费。因此首版实验还必须在 runtime 协议入口 fail closed：若 generator 带 active TreeIter experimental state 且没有 exact reify，`gi_yieldfrom`、`send(non-None)`、`throw`、`close`、suspended deopt 和 instrumentation attach 路径必须拒绝声明生产成功，并通过配置或 guard 让该函数回到原始 generator/no-op 路径。协议操作测试必须覆盖同一 TreeIter 函数在非 allowlisted 配置和 allowlisted 已优化配置下的这些入口；生产 gate 打开后还必须覆盖 allowlisted 且已优化函数在协议敏感操作前 exact deopt/reify 并委派到 CPython 原路径。allowlisted 实验结果只对该 harness 的受控消费方式有效。

### 9.6.7 codegen 寄存器约束

AArch64 codegen 注意事项：

1. `arch::ptr_resolve()` 会 clobber scratch register，不能把同一个 scratch 同时用于保存 `stack_top`。
2. 只使用 `DISALLOWED_REGISTERS` 范围的 scratch clobber 临时值，避免覆盖寄存器分配器分配的活跃值。
3. `SaveCurrentNode` decref 可能调用 `_Py_Dealloc`，该 LIR 指令必须被 postalloc move fold 视为调用或 clobber 屏障。
4. `StateStackPush` 需要先把输入 node/phase 读入安全临时寄存器，再加载 `stack_top`，避免输入分配到同一寄存器被覆盖。

x86_64 codegen 后续注意事项（首版不实现 native translate，只保留 no-op arch gate）：

1. 可使用 `rbp + offsetof(...)` 直接寻址。
2. 固定寄存器如 `rax/rdi/rsi/r13` 的使用必须符合当前 codegen scratch 约定。
3. 后续启用 x86_64 native 路径前，必须先移出 no-op gate，并通过同等 correctness、protocol、deopt 和性能矩阵，编译通过本身不构成支持信号。

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
| `test_tree_iter_state_machine_truthy_guard_default_truthiness` | 原始源码使用 `if child:`，且 exact Node 使用 default truthiness 时，状态机可触发并保持结果一致 |
| `test_tree_iter_state_machine_truthy_guard_custom_truthiness_not_optimized` | exact Node 或 child 类型定义 `__bool__` / `__len__` 时，truthiness guard 不得被当作 `is not None` |
| `test_tree_iter_state_machine_bare_yield_from_none_not_optimized` | 裸 `yield from None` 保留 CPython `TypeError`，不得被状态机跳过 |
| `test_tree_iter_state_machine_split_dict_field_proof` | dict-backed heap object 的 split-dict `LoadField`/`CheckField`/`LoadAttr` 合流能形成 `FieldAccessProof`，状态机使用 fast guard 读取字段 |
| `test_tree_iter_state_machine_field_proof_payload_roundtrip` | `LoadTreeIterField` 的 proof id/payload 在 HIR dump、clone、lowering 和 LIR/codegen 中不丢失；不同 proof id 不被错误 CSE 合并 |
| `test_tree_iter_state_machine_split_dict_fallback_fail_closed` | 字段删除、layout invalid 或 descriptor/property 使 split-dict fallback 可观察时，实验路径 no-op/fail closed，生产路径 invalidate 或 exact deopt/reify |
| `test_tree_iter_state_machine_child_guard_before_state_mutation` | 每次 left/right 动态 load 后，None/default truthiness、exact owner type 和 iterator identity guard 均支配 `StateStackPush`、`TreeIterEnterChild` 和 `SaveCurrentNode` |
| `test_tree_iter_state_machine_non_iterable_child_not_optimized` | 非 iterable child 保留原始异常 |
| `test_tree_iter_state_machine_exact_type_required` | 子类覆盖 `__iter__`、自定义 iterator、descriptor/property 副作用不触发 |
| `test_tree_iter_state_machine_not_triggered_for_non_tree` | 非目标 generator 不触发 |
| `test_tree_iter_state_machine_disabled` | 关闭环境变量时不触发 |
| `test_tree_iter_state_machine_stack_limit` | 原始递归语义可接受深度内触发 heap stack 扩容，不越界、不崩溃 |
| `test_tree_iter_state_machine_depth_budget_deopt` | 超过原始递归 generator 语义边界的极深 skewed tree 在生产路径 exact deopt/reify 或生产 no-op，不因 heap stack silently 成功遍历 |
| `test_tree_iter_state_machine_active_path_cycle_deopt` | self-cycle、right-cycle 等 active-path 回边在生产路径 exact deopt/reify 或生产 no-op；shared subtree 离开当前路径后允许再次进入 |
| `test_tree_iter_state_machine_active_path_transitions` | left、right、kExit、BACKTRACK 的 active-path add/remove 和 depth 增减顺序符合转移表，shared subtree 在离开路径后可再次进入 |
| `test_tree_iter_state_machine_state_allocation_failure` | 强制 `EnsureTreeIterState` 或 `StateStackPush` 扩容失败，确认抛出受控 `MemoryError`，FrameState 可用，current/stack 引用不泄漏 |
| `test_tree_iter_state_machine_completion_clears_state` | 正常 done 路径执行 `ClearTreeIterState`，current/stack owned references 被释放，后续 frame clear/dealloc backstop 幂等 |
| `test_tree_iter_state_machine_gc_cycle` | current/stack 持有引用时 GC 不泄漏、不悬挂 |
| `test_tree_iter_state_machine_protocol_gate` | 首版实验路径不声明 `gi_yieldfrom`、`send(non-None)`、`throw`、`close` 生产支持；生产配置在协议 gate 未闭合时 no-op |
| `test_tree_iter_state_machine_experimental_protocol_fail_closed` | allowlisted 且已优化的实验 TreeIter 在 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`、suspended deopt、instrumentation attach 上 fail closed，不静默继续执行，也不伪装为生产协议等价 |
| `test_tree_iter_state_machine_experimental_abort_helper` | 实验 fail-closed helper 清理 TreeIterState、失效 optimized entry，并在可抛异常入口产生确定性内部 bailout；finalize/tp_clear 只清理和失效 |
| `test_tree_iter_state_machine_protocol_ops_reify` | 生产 gate 打开后，allowlisted 且已优化的 TreeIter 在 `gi_yieldfrom`、`send(non-None)`、`throw`、`close` 前 exact deopt/reify 并委派 CPython 原路径 |
| `test_tree_iter_state_machine_experimental_allowlist` | 仅 TreeIter 专用 exact allowlisted 的目标 code object 能在实验配置下触发状态机；缺少 allowlist、wildcard jitlist、普通 `compile_after_n_calls` 或低 auto 阈值时 no-op |
| `test_tree_iter_state_machine_protocol_ops_not_allowlisted` | 同一 TreeIter 函数在非 allowlisted 配置下执行 `gi_yieldfrom`、`send(non-None)`、`throw`、`close`，确认走原始 generator 路径 |
| `test_tree_iter_state_machine_deopt_gate` | suspended generator deopt / instrumentation attach gate 未闭合时生产配置 no-op |
| `test_tree_iter_state_machine_reify_feasibility_artifact` | 每个 phase/stack/kExit 形态都有可执行 reify mapping；缺少 resume offset、FrameState、`gi_yieldfrom` 或引用转移说明时 production gate false |
| `test_tree_iter_state_machine_arch_gate` | 未验证架构配置开启仍 no-op |
| `test_tree_iter_state_machine_admission_artifact` | 生产 artifact verifier 能检测 HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt 五个纵切面齐备，且 stale commit/HIR/hash 失败关闭 |
| `test_tree_iter_state_machine_generators_workload_gate` | M1.5 `generators` workload artifact 必须记录来源、owner、数据形态、code object、命令、allowlist 和稳定性；本地 TreeIter 样例和 pivot 候选 workload 不能替代本 V2 gate |
| `test_tree_iter_state_machine_jit_workload_scope` | 21 个 JIT 用例列表中，只有 `generators` 可声明 TreeIter 直接性能目标；`nqueens` 等相邻 generator 和 OO-heavy 用例只作为非目标回归或后续复用方向 |

测试必须显式启用：

```text
PYTHONJIT=1
PYTHONJITTREEITERSTATEMACHINE=1
```

性能测试必须使用 TreeIter 专用 exact code-object allowlist/jitlist 或等价专用 harness token；普通全局 `cinderx.jit.compile_after_n_calls()` 和低 `PYTHONJITAUTO` 阈值只能作为普通 JIT 编译机制，不能单独触发状态机 pass。旧分支证明低阈值可能编译 stdlib 函数并引入无关崩溃。

生产 gate 测试矩阵：

| 维度 | 要求 |
| ---- | ---- |
| correctness | debug/release 均跑 guarded None、truthiness default、truthiness custom negative、bare None、非 iterable、exact type、子类覆盖、split-dict guard/fallback、active-path 环检测、shared subtree、原始递归深度边界 |
| protocol | 生产启用前覆盖 `gi_yieldfrom`、`send`、`throw`、`close`、StopIteration value、异常上下文；首版实验只断言生产 gate no-op |
| lifecycle | 覆盖 GC cycle、tp_clear、finalize、free-list 复用、正常完成、异常退出 |
| deopt | 生产启用前覆盖 suspended generator deopt、instrumentation attach、deopt 后继续执行；首版实验只断言生产 gate no-op |
| platform | 每个启用架构独立通过；未验证架构 no-op |
| performance | 指定 benchmark 命令、重复次数、统计阈值和非目标 generator 回退阈值 |

# 10 DFX分析

## 10.1 可靠性分析

| 风险 | 详细设计约束 |
| ---- | ------------ |
| 状态字段隐藏引用导致 GC 漏扫 | `jitgen_traverse` 访问 current node、stack node 和生产 active-path 中 owned node |
| free-list 复用旧状态 | 分配时初始化，释放时 clear |
| borrowed ref 被 `XDecref` 释放 | Load 返回前 `INCREF`，Pop 转移 owned reference，SaveCurrentNode 先持有新 node 再释放旧 current |
| 状态分配失败隐藏在无异常边 LIR 中 | `EnsureTreeIterState` / `StateStackPush` 必须携带或绑定 `FrameState`，失败走受控 `MemoryError` |
| `InitialYield` clobber self | 初始化保存插入到 `InitialYield` 前 |
| HIR 优化破坏状态转换 | 写状态和 refcount 指令保守副作用 |
| 原生 LIR 被 postalloc 错误 fold | 增加 clobber 屏障和中间寄存器使用检查 |
| 裸 `yield from None` 语义漂移 | 只匹配原始 guard；裸 yield-from 保留 `GetIter`/TypeError |
| truthiness 语义漂移 | 只有 exact default truthiness 证明成立时才能把 `if child:` 视为 `child is not None`；自定义 `__bool__` / `__len__` 负例必须 no-op |
| split-dict 字段 fallback 被删除 | `FieldAccessProof` 必须记录 fast guard、fallback shape 和 runtime failure action；guard 失败或 fallback 可观察时实验 fail closed、生产 invalidate 或 exact deopt/reify |
| generator 协议漂移 | 首版实验不声明生产支持；生产启用前必须在可观察协议操作前 exact deopt/reify |
| release 栈容量不足 | push 前在原始递归语义深度预算内动态检查并扩容 heap stack，扩容失败走受控错误路径 |
| active-path 回边被当成普通树遍历 | 进入 child 前检测当前路径回边，生产路径 exact deopt/reify；不做全图 visited 去重，避免误跳过 shared subtree |
| heap stack 隐式支持更深递归树 | 维护原始递归语义深度预算，越界 exact deopt/reify 或生产 no-op |

## 10.2 异常处理设计

状态机首版只优化无额外异常语义的 TreeIter 实验模式；生产启用必须另行闭合协议和 deopt：

1. `LoadTreeIterField(left/right/value proof)` 仍保留原 HIR 的 guard/check/fallback 语义边界，且不得删除 `GET_YIELD_FROM_ITER` 中仍可观察的 TypeError/coroutine rejection 语义；slot/member 与 split-dict proof 均必须有明确 runtime failure action。
2. 首版实验 `YieldValue` 不声明 yield-from 可观察协议等价；生产配置在 `gi_yieldfrom`、`send/throw/close` exact deopt/reify 未闭合前 no-op。
3. 对无法精确恢复的 deopt 场景，生产配置应拒绝优化；实验配置必须通过 gate 测试证明不会被误标为生产可用。
4. 对 `_Py_Dealloc` 罕见路径，codegen 可以调用 C helper；该指令必须标记为 clobber 边界。
5. 栈 underflow/overflow 在 release 构建必须有 heap-backed 动态安全路径；debug `JIT_DCHECK` 只能作为额外诊断。
6. active-path 回边和原始递归深度越界在生产路径必须 exact deopt/reify；缺少 reify contract 时 matcher 必须 no-match 或 production gate no-op。
7. 状态对象分配、heap stack 扩容和引用提交顺序按 9.2.5 的状态 op 异常契约执行；缺少可复用 `FrameState` 时 matcher 必须 no-match。

## 10.3 性能分析

性能收益来自四点：

| 来源 | 说明 |
| ---- | ---- |
| 消除递归 generator yield-from 帧切换 | 不再为 left/right 子树递归创建和恢复 generator 帧 |
| 支持真实 dict-backed TreeIter HIR | 通过 truthiness 等价性和 split-dict `FieldAccessProof` 覆盖 pyperformance `generators` 当前 master 形态，避免只优化 slot-only micro benchmark |
| Heap-backed TreeIterState | footer 只保存状态指针，current/phase/stack 通过一次指针解引用访问 |
| 原生 LIR/codegen | 避免热路径 C runtime helper 调用 |

首版实验性能验收口径：

```text
baseline: 当前 master + PYTHONJITTREEITERSTATEMACHINE=0
target:   当前 master + PYTHONJITTREEITERSTATEMACHINE=1（实验路径）
case:     已通过当前 master split-dict/truthiness 实验准入和 workload 接受标准的 pyperformance generators Tree.__iter__，
          若该用例无法准入，停止在 matcher 研究，不进入 M2-M4；
          非合成、生产等价 TreeIter workload 只能记录为后续 pivot 候选，不能替代本 V2 的 M4 成功标准。
```

depth 1-12 micro benchmark 只允许作为 correctness、栈扩容和 codegen smoke evidence；不得作为 M4 性能 gate 的目标 workload，也不得用于声明优化收益。

旧分支记录的 4-12x 只作为预期参考，重实现必须重新在当前 master 上测量。M4/M5 的性能 gate 固定为：

| Gate | 要求 |
| ---- | ---- |
| 实验 workload 准入 | pyperformance `generators` 必须满足实验准入和 workload 接受标准；否则性能 gate 自动失败，并停止 M2-M4 状态机实现。替代 workload 只能作为后续 pivot 输入 |
| 实验目标收益 | AArch64 release 构建中，target 相对 baseline 的 median wall-time speedup 必须 >= 2.0x，且 bootstrap 95% CI 下界必须 > 1.25x |
| 生产目标收益 | 协议/deopt 闭合后重新测量，target 相对 feature-off baseline 的 median speedup 必须 >= 1.5x，且 95% CI 下界必须 > 1.10x |
| 非目标回退 | M4 至少覆盖 `generators` 以外的 generator-heavy micro/pyperformance 子集；任一非目标 generator median slowdown > 5% 或 95% CI 上界显示显著回退时 gate 失败。21 个 JIT 用例列表作为 M5/default rollout 前的更宽回归矩阵 |
| 稳定性 | 同一平台至少 20 个 process-level runs；剔除规则、均值/中位数、方差和异常值处理方式写入 artifact，目标用例 coefficient of variation > 10% 时需重跑或解释 |
| 口径 | 明确 CPython/CinderX、JIT on/off、feature on/off、jitlist/allowlist、环境变量、CPU affinity/频率策略，不混用口径基线和提交基线 |
| artifact | 实验性能提交 `run.json` / `speedup.json` 或等价原始结果、执行命令、平台指纹、allowlist 和 workload artifact；生产性能还必须提交五个纵切面 admission verifier 结果；缺任一项 gate 失败 |

## 10.4 安全和韧性分析

本功能不新增外部权限、文件、网络或用户输入解析。安全重点是 native 内存安全：

1. `state_stack` 访问必须有容量边界、语义深度预算和 heap 扩容失败路径。
2. `GenDataFooter.tree_iter_state` 必须初始化为空，避免读未初始化指针。
3. 所有 `PyObject*` 状态必须参与 GC traverse 和 clear。
4. 架构相关 codegen 默认只在已验证平台开启。
5. 配置项保留快速关闭能力，便于线上回退和性能二分。
6. CPython generator/yield-from 可观察协议、active-path 环检测和原始递归深度边界必须作为生产 correctness 边界，而不是性能测试附属项；首版实验结果不得替代生产 gate。

## 10.5 决策项承接状态

以下问题来自文档审查的 manual 类意见，均已由功能设计讨论确定。详细设计按实现层级承接，后续实现不得重新打开为互斥方案选择：

| 编号 | 状态 | 详细设计承接 |
| ---- | ---- | ------------ |
| D-GJIT-SM-001 | 已决：生产路径在 `gi_yieldfrom`、`send(non-None)`、`throw`、`close` 等协议敏感操作前 exact deopt/reify；无法 reify 时生产 no-op | 9.3.2、9.5.2.4、9.6.4、9.6.6 和 9.6.9 定义 gate、M1.6 reify 可表示性 artifact、reify contract 和协议操作测试 |
| D-GJIT-SM-002 | 已决：生产范围为有限无环树；运行时用 active-path 检测回边，并保持原始递归 generator 的深度语义边界 | 9.2.4、9.2.5、9.3.2、9.4.1.3、9.5.2.4 和 9.6.9 定义 active-path/depth 转移表、kExit marker、shared subtree 行为和测试 |
| D-GJIT-SM-003 | 已决：生产准入按五个纵切面 artifact 组织：HIR 结构、owner/child exactness、field layout、iterator identity、失效/deopt | 9.2.2、9.3.1、9.5.2.4、9.6.9 和 10.3 定义 artifact schema、fail-closed verifier、gate 名称和性能准入口径 |
| D-GJIT-SM-004 | 已决：直接性能目标为 `generators` 所代表的递归对象树 `yield from`，且 M4 必须以该用例达标作为 V2 成功条件；实现以 `FieldAccessProof`、truthiness 等价性和 exactness/layout/deopt 证明匹配，不按 benchmark 名称硬编码；其它 OO-heavy 用例只承接为后续复用方向和本轮回归边界，替代 workload 只能触发后续 pivot 决策 | 9.1.3、9.2.2、9.5.1、9.6.9 和 10.3 定义字段证明、21 个 JIT 用例覆盖边界、性能 gate 和非目标回归要求 |
