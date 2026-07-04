# M9 第三轮预演日志：有机 deopt-resume 修复（正确性层歼灭战）

日期：2026-07-04　分支：`dryrun/m9-perf-r3`　基线：`dryrun-311-base@9a74d3dd7`（M9R2 合入后）

目标（用户设定）：19 项基准在 auto=2 / warmup=3 口径下全部正常完成。
入口是 M9R2 移交的第一优先项"有机 deopt-resume 修复"；实际展开后
发现所谓 deopt-resume 案是**六个独立根因**的合流（第六个由修复五
揭开——被运行态残留掩护的幻影槽 UAF，见 §三），本轮逐一定案修复，
19/19 基准全绿。

## 一、前五个根因与修复（全部 `<0x030C0000` 版本门内）

### 1. deopt 物化的方法对槽序与 3.11 解释器约定不符（deltablue SEGV 根因）

- **约定冲突**：3.11 解释器 CALL 窗口栈序为 `[meth_or_null,
  self_or_callable, args...]`（LOAD_METHOD 未命中方法形态时 NULL 标志
  位在**下**槽）；JIT 内部沿用 3.12+ 约定 `(callable, self_or_null)`
  （NULL 在**上**槽）。纯 JIT 执行时 `JITRT_Call` 动态剥离哨兵，自洽；
  **deopt 时 `reifyStack` 把 JIT 序原样写进解释器帧**，非方法形态下
  两槽错位。恢复后解释器以 `PEEK(oparg+2)` 判定 `is_meth`，把 callable
  误当方法标志 → bound method 再前置一次 self → 元数与实参全错。
- **实测证据链**：deltablue `ScaleConstraint.recalculate` 在
  LOAD_METHOD(47)..CALL(72) 窗口内因 `dict values check` 守卫失败
  deopt（idx 58），`Strength.weakest_of`（classmethod，generic 路径
  返回 bound method + NULL 哨兵）pair 活在栈上；gdb 实测最终调入
  底层函数的实参向量为 `[cls, NULL, s1, s2]`（4 参对 3 参），经
  元数修补 stub 落入解释器兜底后对 NULL 实参 SEGV。
- **修复**（deopt.cpp reifyStack）：方法形态（self≠NULL）两约定字面
  同序无需处理；非方法形态按运行时值交换两槽为 `[NULL, callable]`。
  交换锚点复用既有 `LiveValue::isLoadMethodResult()` 标记（≤3.10
  时代 Py_None→NULL 修补的同一机制）；3.11 分支同时停用该
  Py_None→NULL 替换（3.11 归一化后 callable==None 是合法值——属性
  本身为 None 的场合）。静态定序不可行正是 3.12 更改字节码约定的
  原因，运行时条件交换是唯一修法。
- **M9 立案时的 `prev_instr==-1`（enum/importlib 现场）系下游赃物**：
  被污染的调用新建的帧在执行首条指令前即崩，非独立缺陷。

### 2. LOAD_GLOBAL 守卫 FrameState 含预推的 NULL 哨兵（sqlglot TypeError 根因）

- 3.11 `LOAD_GLOBAL`（oparg&1）语义为"推 NULL + 推全局值"。
  `emitLoadGlobal` 先 `emitPushNull` 再发射带 Guard 的
  `tryEmitLoadGlobalModuleValue311`，守卫 FrameState 里已含该 NULL；
  deopt 恢复**重执行整条 LOAD_GLOBAL** 又推一次 → 栈上多一个哨兵，
  后续调用窗口整体错位。sqlglot `_quotes_to_format` 实测：deopt 记录
  仅一条（`LOAD_GLOBAL_MODULE: _convert_quotes`，bc=12，import 期
  模块字典插键使 dk_version 漂移），逐指令推演 `[compfunc, NULL,
  NULL, _cq, arr]` 走到字典推导调用点时 `function=PEEK` 取到
  dict_itemiterator → `TypeError: 'dict_itemiterator' object is not
  callable`，与观测逐字吻合。
- **修复**（builder.cpp emitLoadGlobal）：NULL 推栈移到（可能 deopt
  的）加载发射之后，守卫 FrameState 即为指令边界前状态；模拟栈最终
  压栈顺序不变。**通则**：builder 内联发射的守卫，其 FrameState 必须
  等价于"从该字节码重新执行"的恢复状态（simplify 路径继承边界
  Snapshot 天然满足；builder 快路径需自查）。
- 症状对 gdb/导入顺序敏感的机理：deopt 是否发生取决于编译时刻与
  模块字典演化的相对时序，与 M9R2"崩溃集随阈值漂移"同源。

### 3. GuardTypeRemoval 删除 WITH_VALUES 方法快路径的接收者守卫（raytrace 错派发根因）

- `tryEmitLoadMethodWithValues311` 的三道守卫（类型版本/managed
  dict/keys 版本）检查的都是**编译期钉住的 owner 类型常量**；接收者
  恰为该类型的语义依赖只体现在入口 GuardType 上。该依赖未用
  `UseType` 钉住 → GuardTypeRemoval 按"无使用者需要精化类型"将
  GuardType 判定可删（最终 HIR 实证无接收者守卫）→ 异类型接收者
  畅通拿走缓存的方法常量。raytrace 现场：多态 `o.intersectionTime`
  循环，Sphere 实例执行了 Halfspace.intersectionTime（callee 入口
  的实参类型 GuardType deopt 后解释器继续错误方法体 →
  AttributeError 'Sphere' has no attribute 'normal'）。
- **修复**：GuardType 后无条件 `UseType(receiver,
  expected_receiver_type)`（hir.h 中 UseType 的注释原文即为此场景）。
- 教训：此前的针对性探针只测了"同类型改版本"轴，漏了"异类型
  接收者"轴——守卫类用例必须逐轴变异。

### 4. CompiledFunction 借用函数指针无 3.11 摘除机制（spectral_norm SEGV 根因）

- `CompiledFunction::functions_` 存借用指针，靠 3.12+ function
  watcher 的 DEALLOC 事件在函数死亡时摘除；3.11 无 function
  watcher（M7 已定案 watcher 空桩家族，此为漏网第二处）→ 函数死亡
  不摘除 → `clear()` 的 vectorcall 复位循环对已释放函数对象 UAF
  写。spectral_norm 现场：短命编译函数死亡触发其 __dict__ 中
  CompiledFunction 析构，`getInterpretedVectorcall` 读 `func_code`
  （已被 func_clear 清空）→ SEGV。
- **修复**（compiled_function.cpp，3.11 门内）：functions_ 持强引用
  并在 `traverse` 上报——与函数 __dict__ 中对 CompiledFunction 的
  强引用构成环，由 GC 统一回收（clear 即 tp_clear 断环点）；clear
  复位入口时不再读 func_code（函数可能已过 tp_clear；3.11 无 Static
  Python，恒为 `Ci_PyFunction_Vectorcall`）。
- **代价与移交**：编译过的函数只能经 GC 环回收（refcount 即时回收
  失效）；正式实现建议评估 watcher 等价物（per-func attachment 对象
  的 dealloc 钩子）。`deopted_funcs_` 的悬垂键问题同源未修（当前
  基准不触发，记档）。refcount 矩阵四组零漂移证明改动无引用泄漏。

### 5. jitgen_am_send 缺 stock gen_send_ex2 的完成路径镜像（sqlglot "generator already executing" 根因，M8 移交案销案）

- 3.11 中生成器完成路径的四项职责在 stock `gen_send_ex2` 尾部：
  ① EXECUTING→COMPLETED 归一化；② exc_info 恢复；③ PEP 479 洗白
  （StopIteration→RuntimeError）；④ 完成后置 CLEARED 并清帧。
  `jitgen_am_send` 是其 JIT 替身，原实现只有 JIT_DCHECK 没有镜像
  （release 构建全部失效）——深度 deopt 后经解释器完成的路径上
  没有任何一方执行这些职责 → 运行态残留 FRAME_EXECUTING，后续一切
  操作报 "generator already executing"；StopIteration 原样外漏。
  **fallback 手写镜像第六抓**。
- **修复**：四项按 stock 逐字镜像补齐（<0x030C 门内；完成清帧仅在
  "完成而未 CLEARED"即解释器完成路径上执行，避免与 JIT 原生完成
  路径重复释放）。最小复现（异常退出后 gi_running/三次 next/PEP479
  类型）修复前后对照全部转为 stock 语义。

## 二、门禁与回归

- diffgate 主语料 **922 用例（918 + 新蒸馏 4 例）0 失败**（M7 以来
  的全绿口径维持，新蒸馏用例首跑即绿）；
- generators 语料维持 M8 已知集合（locals 缺口 = M3/M6 既定移交；
  序列依赖 SIGILL 与 pep479 序列毒 = M8 既定移交，两者隔离运行均
  全绿——pep479 的 PEP 479 洗白与运行态复位本体已由修复 5 销案，
  残余崩溃属序列毒同盆），**存量 0 新增**；新增
  case_gen_exception_exit_state（jit 模式绿；jit_deopt 序列毒形态
  入基线同盆记档），基线快照 m9r3-gen-baseline.json；
- refcount 矩阵四组（calls/operators/hotloops/frames）interp vs
  jit **0 漂移**（生命周期改动的专项验证）；
- 3.14 反向：ninja 全量编译 0 错 + 冒烟（普通函数/生成器/
  classmethod 派发 + LWF 模式确认）通过。

## 三、根因 6：JitGen dealloc 的幻影 LWF 帧头槽（scimark SIGILL /
nqueens SEGV / M8"序列依赖 SIGILL"总根因）

定位过程曲折（值得记档的取证链）：

- scimark SIGILL：bare 100% 复现、gdb/ptrace 下不复现；核心转储显示
  某 JIT 函数尾声的 `ret`（0xd65f03c0）变为 0xd65f03be——先按"分支
  imm26 原地调 -2"错误怀疑 6 月进树的 aarch64 分支松弛；
  `PYTHONJITMULTIPLECODESECTIONS=1` 二分曾支持该假说。
- nqueens 修复回归（见下）给出真相：`deopt_jit_gen_object_only` 的
  `#ifdef ENABLE_LIGHTWEIGHT_FRAMES` 块在 `<0x030E` 分支经
  `jitFrameGetFunction` 读"帧前 LWF 头槽"——**3.11 物化帧没有这个
  头**，读到的是 JitGen 结构邻居字节。完成态（FRAME_CLEARED）分支
  对该幻影"函数指针"做 `Py_XDECREF`：指针恰好落在 JIT 代码区时，
  `ob_refcnt` 位置对准某条指令字，**每次完成生成器 dealloc 使该
  指令字减 1**——scimark 的 ret−2 = 两次幻影 decref 的账，SIGILL/
  SEGV/无症状取决于幻影指针落点（布局函数），gdb/mcs/序列敏感性
  全部由此解释。asmjit 松弛无罪。
- **修复**：该块版本门收窄为
  `defined(ENABLE_LIGHTWEIGHT_FRAMES) && PY_VERSION_HEX >= 0x030C0000`
  （3.11 的 f_func 由 _PyFrame_Clear/原生 teardown 释放，无需此块）。
- **修复后实证**：scimark 两种布局（mcs=0/1）全绿；nqueens 全口径
  全绿；M8"序列依赖 SIGILL"与 generators 语料序列毒同案销案。
- **掩护链**（M8"双缺陷互掩"的续集）：修复 5 之前，经解释器完成的
  deopt 生成器带着 FRAME_EXECUTING 残留走 `!= FRAME_CLEARED` 分支，
  恰好绕开幻影 XDECREF；修复运行态后该家族首次走进完成态分支，
  nqueens 立即回归——跨版本移植中"修复揭开被掩护缺陷"是常态，
  门禁必须在每个修复后全量重跑。

## 四、水位（口径：auto=2 / warmup=3 / p3v5 / 默认布局配置，manylinux aarch64 容器）

**19/19 基准全部正常完成，0 失败**（M9 目标达成；数据存档
m9r3-ab-summary.json）：

| 基准 | ratio（stock/jit，>1 为 JIT 更快） |
|---|---|
| nbody | **1.222x**（唯一转正，拆箱路径） |
| fannkuch | 0.983x |
| float | 0.885x |
| sqlglot_v2_parse / transpile | 0.826x / 0.796x |
| regex_compile | 0.823x |
| spectral_norm | 0.780x |
| unpickle_pure_python | 0.758x |
| nqueens | 0.755x |
| richards_super / richards | 0.724x / 0.626x |
| generators | 0.706x |
| scimark | 0.671x |
| chaos | 0.668x |
| go | 0.665x |
| pickle_pure_python | 0.656x |
| hexiom | 0.567x |
| raytrace | 0.542x |
| deltablue | 0.493x |
| **几何均值（19 项）** | **0.729x** |

口径说明：与 R2 t2 的 0.762x（更少可比项）不可直接对比——本轮
纳入的原崩溃项（deltablue/raytrace/hexiom 等）恰是 call/attr 密集、
有机 deopt 频繁的最慢组，混入后整体均值下移属构成效应。该组的
减速构成（正确但昂贵的 deopt-恢复路径 + M9R1 归因的结构三项）是
正式 M9 热点收敛的直接输入；性能打磨杠杆（LWF、IC 内联快路径）
判断维持 R2 结论不变。

## 五、LWF=on 口径说明（需拍板）

目标口径中的 LWF=on 在 3.11 上当前**不可满足**：
`PYTHONJITLIGHTWEIGHTFRAME=1` 实测 init 即 SEGV——LWF 机器编译进了
二进制但对 3.11 帧布局无感知（M3 定案：字段表按 3.12+ 结构写，
跳过 frame_obj/stacktop/is_entry），D4 亦明确 LWF 翻转为 v1.1 独立
里程碑。本轮 A/B 按物化帧口径交付；LWF-on-3.11 是独立移植件
（帧建/物化/reify/gen 全链路的 3.11 布局适配），建议作为 M9 后续
专项排期评估。

## 六、移交清单更新

- ~~scimark/序列 SIGILL~~：根因 6 销案（asmjit 松弛无罪；ASAN 专项
  的紧迫性由"抓同类幻影读写"接续——jitFrame* 头部辅助函数族在
  3.11 的其余调用点建议正式开发全量审计）；
- CompiledFunction 生命周期：watcher 等价物设计 + deopted_funcs_ 悬垂键；
- builder 内联守卫 FrameState 通则：正式开发按"指令边界前状态"
  逐点审计（本轮已修 LOAD_GLOBAL；LOAD_METHOD_WITH_VALUES 实测锚点
  正确）；
- diffgate runner 已增"编译前解释热身"（量化后编译时序，见 §二），
  正式门禁应保留两种时序档位（未量化编译 ≈ 低阈值 auto 形态）；
- LWF-on-3.11 翻转评估（独立移植件，见 §五）。

## 七、工时

本轮约 4.5 小时（复现与定位约 2/3：五根因逐案 gdb/核心转储/HIR
对照；修复与门禁回归约 1/3）。M9 三轮累计约 10 小时。
