# M10 第二十三轮：libtest 扩充首测与两处关机窗口崩溃修复

日期：2026-07-08　分支：`dryrun/m10-libtest-expand`　基线：!67。
背景：libtest 门禁自 M2 起固定 26 个语言核心模块；本轮按"战役改动面
优先"扩充两档共 23 个新模块并首次双臂差分（auto=4 交付口径）。

## 一、扩充清单与首测结果

- 第一档（12，MR 级门禁）：copy / pickle / coroutines / asyncgen /
  yield_from / except_star / exception_group / extcall / funcattrs /
  code / import / positional_only_arg——逐项对应本战役改动
  （exc-fuse、三阀摘除、[P7]、生成器策略、调用直派、缺省补齐、
  [P8]、kwargs-bind）。
- 第二档（11，收口链）：descr / metaclass / isinstance / traceback /
  frame / sys_settrace / gc / weakref / iter / functools / binop。
- 首测 20/23 双臂一致；3 项 DIVERGE：test_coroutines（SIGSEGV）、
  test_code（SIGSEGV）、test_descr（FAIL）。三案根因互不相同，
  且**全部与 JIT 编译无关**（auto=0 零编译复现）——扩充首日即命中
  两处运行时集成层真崩溃，验证了按改动面选模块的准星。

## 二、案一：_PyGen_yf 兜底借用形返回（test_coroutines）

**形态**：await 一只已在 await 中的协程（`test_await_15`），
RuntimeError 正常抛出，进程在退出阶段段错；仅需失败的二次 await
+ waiter 临时对象死亡 + 协程活到关机周期 GC 三条件（四变体 3/3
复现；waiter 存活或显式 close 则不崩——后者为内存未复用的假阴性）。

**定位链**：faulthandler → gdb 栈（`_PyGen_Finalize`→`gen_close`→
frame_dealloc 处 GC 链表对 NULL 写）→ 硬件观察点逐写审计 → 捕获
生成器在 vendored `ceval.c:2580`（GET_AWAITABLE 的 `Py_DECREF(yf)`）
被析构。vanilla 语义下该 DECREF 仅归还 `_PyGen_yf` 刚取的新引用，
致死即上游记账已差一。

**根因**：运行时不导出内部符号 `_PyGen_yf`，.so 内所有调用绑定到
`UpstreamBorrow/borrowed-3.11-fallback.c` 的兜底实现——其返回**借用
引用**，而 vanilla 约定（Objects/genobject.c 同名函数 `Py_INCREF`
后返回）是**新引用**。三处调用方均按新引用消费：vendored
GET_AWAITABLE、StaticPython `awaitable.c`（同形 `Py_DECREF`，同一
潜在崩溃面）、`JitGen_yf` 委托（JIT 编译产物经 refcount pass 按持有
引用记账，编译态 await 已 await 协程为第三张脸）。过度递减将挂起
协程栈上的 awaitee 杀死为悬垂指针，关机周期 GC 遍历引爆。

**修复**：兜底改为 `Py_NewRef(yf)`（非 None 分支），与 vanilla 约定
对齐；`JitGenObject::yieldFrom()` 本已 `Py_XINCREF`，全仓约定就此
统一。一处修复三面同愈。

## 三、案二：co_extra 数组全量拉满扩大第三方 freefunc 引爆面（test_code）

**形态**：28 个用例全过、regrtest 报 SUCCESS 后，关机最终 GC 段错；
不选 CoExtra 子测试也崩（其 ctypes freefunc 在类体定义时即注册）。

**定位链**：gdb（"Garbage-collecting / no Python frame"，ffi 闭包
调入 Python）→ 最小复现分型：带 extra 的 code 若活到关机，**纯
stock 也 3/3 崩**（关机窗口执行 Python 级 freefunc，globals 已清，
STORE_GLOBAL 对已清字典写入——上游潜伏雷，基版类，归因结案）；
`del` 后 code 即死则 stock 干净而 cinderx 仍崩。

**根因**：vanilla `code_dealloc` 对 `ce_size` 覆盖的**每个索引无条件
调用注册 freefunc（NULL 槽亦然）**；`_PyCode_SetExtra` 把数组按
interp 的 `co_extra_user_count` 全量拉满。本运行时给几乎所有活跃
code 设 extra（计数/裁决），于是所有被碰过的 code 的数组都覆盖到
第三方索引——关机晚期死亡的任意 code 都会调起 test_code 注册的
Python 回调，踩中上游潜伏雷。

**修复**：新增 `Ci_code_extra_set_min_311`（写侧同布局镜像，与既有
只读镜像同一版本锚定论证）：数组只扩到本索引+1。两处调用方
（`Common/code.cpp` 的 codeExtra 建立、`Jit/osr.cpp` 的回边计数器）
切换；本运行时两个 freefunc 均为裸 `PyMem_Free`，teardown 安全。
效果：我们碰过的 code 永不触发第三方 freefunc；第三方自己的 extra
生命周期与 stock 完全一致。

## 四、案三：test_descr（test_slots 的 gc.get_objects 恒等断言）

SF-572567 查漏检查在两次 `gc.get_objects()` 计数间执行 10 次
`g==g`——auto=4 下 `G.__eq__` 恰在窗口内跨过编译阈值，编译簿记
驻留 +3 对象（恒定，不随迭代放大，非泄漏）；auto=24/auto=0 均过。
判定：**环境性已知分歧**，进扩充基线吸收，不修。

## 五、判据与工件

- 最小复现：await15 形态 0/3 崩（auto=0 与 auto=4+求值器）；
  coextra del 形态 0/3 崩（双配置）；test_coroutines / test_code
  全模块双阈值（4/24）全过，test_code 连跑 3 次稳定。
- 扩充 23 模块复跑：仅 test_descr 1 项已知分歧（入基线）。
- 原 26 模块回归：通过（test_builtin 对基线转 FIXED，test_scope
  既档待旁路分支）。
- 门禁：冒烟 11 件全过 + 新增 smoke_libtest_expand（两形态各 3 发）；
  diffgate 940 案 0 新增（4 转好）；**RCM 六组双模全等 CLEAN**
  （引用语义改动红线）；exc_inject_fuzz 64 发 0 崩。
- 工件：`ci_pipeline/diffgate/libtest_modules_expanded.txt`、
  `baselines/cp311-libtest-expanded.json`、
  `docs/dryrun/smoke/smoke_libtest_expand.py`。

## 六、第三波扩充（A+B 档，2026-07-08 追录）

A 档语言面扫尾 17（itertools/richcmp/fstring/dataclasses/
contextlib_async/sort/index/long/bool/complex/abc/class/subclassinit/
enumerate/range/format/bytes）+ B 档语义可见性专项 6（finalization/
dis/inspect/sys_setprofile/cprofile/enum），auto=4 双臂差分。
结果 21/23 一致；test_finalization 通过（本轮析构面修复经受住最强
专项）；两项 DIVERGE 均为 FAIL（非崩溃），归因如下：

**test_dis（test_loop_quicken）= 特化拒填家族的 dis 可见投影**。
期望特化为 CALL_PY_WITH_DEFAULTS 的位点在本运行时停在
CALL_ADAPTIVE；加热 500 次不转化、auto=0（零编译、仅 init）同样
拒绝——函数入口包装使上游 specialize_py_call 拒填，即劣化归因轮
记档的"特化拒填"共因在反汇编断言面的显形。语义行为不变
（CALL_ADAPTIVE 正常执行），判定机制性已知分歧，入基线。

**test_inspect（test_stack）= UpdatePrevInstr 行去重的编译帧
mid-call positions 错列**。fodder 的 eggs 帧在调用中途报告列跨度
9..16（行首指令 LOAD_GLOBAL 的跨度）而非 9..24（完整调用表达式）。
定界三证：解释态两形复现均正确；auto=24 与 auto=0 均通过（纯编译
帧面）；insert_update_prev_instr.cpp:122 确认去重键为行号——行内
仅首指令处更新 prev_instr。主仓同病已修（去重键改 bc offset），
311 因簿记密度代价（参见第 D 轮 UpdatePrevInstr 上界 −2.3% 测量）
未回迁。判定已知家族入基线；**修复候选记档：回迁 bc-offset 去重，
落地前须同味 A/B 定价**（每编译函数簿记密度上升，方向与 D 轮相反）。

清单与基线随本节更新：libtest_modules_expanded.txt 增至 46 模块，
基线增 libtest:test_dis、libtest:test_inspect 两项（合计 3 项已知
分歧）。C 档（asyncio/threading/io/typing）维持夜间带定位，未挂
MR 门禁。第三波后差分门禁覆盖 72/407，解释器相关面基本饱和，
后续增补边际信号趋零。

### C 档夜间带首测（2026-07-08 追录）

test_asyncio（整包）/test_threading/test_io/test_typing，auto=4
双臂、单模块超时 1200s：**4/4 一致，零分歧**。asyncio 整包为
[P7]/协程机器与真实事件循环的压测面，threading 覆盖编译队列与
IC 变异的并发面，均无信号。清单固化为
`ci_pipeline/diffgate/libtest_modules_nightly.txt`（无需独立基线），
定位收口链/夜间独立跑。至此差分门禁覆盖 76/407：MR 级 46 +
基础 26（部分重叠）+ 夜间带 4，解释器相关面收官。

## 七、遗留与教训

- **兜底符号约定审计**：borrowed-3.11-fallback.c 是"vendored 基版
  与运行时差配"缺陷面的近亲——我们自写的内部符号兜底与 vanilla
  约定的逐函数比对值得一次系统性扫描（本轮仅证 _PyCoro_GetAwaitableIter
  干净）。
- 关机窗口执行 Python 级 co_extra freefunc 为上游潜伏雷（纯 stock
  可复现），基版类归因结案，不投修复。
- 第三档模块（asyncio 全量/typing/网络族）维持不进清单。
- 教训：①差分门禁的白面孔模块可能藏多年老债，扩前先跑双臂裸测采
  基线；②关机顺序敏感的崩溃对无关代码位移极敏感（案一的 E 变体
  假阴性），变体判定必须 N≥3；③兜底实现内部内部符号时，引用约定
  与 vanilla 的一致性和布局一致性同等重要。
