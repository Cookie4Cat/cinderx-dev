# M9 收尾：全表面 SEGV 四案归因（[P5] 补丁与残案立档）

日期：2026-07-04　分支：`dryrun/m9-testsuites`（测试接入轮同支继续）
输入：基础 libtest 差分（26 模块，全表面 auto=24）首基线的 5 发散——
test_builtin / test_list / test_tuple / test_exceptions 四个 JIT 侧
SEGV + test_scope FAIL。时间盒半天，出口=根因家族+蒸馏语料。

## 一、根因 A（四案中三案）：借用实参约定 vs WITH_EXCEPT_START 的裸借用 tb

**取证链**（test_tuple 现场，全链确定性）：

1. faulthandler：四案全部崩在异常抛出/捕获路径（getitem_error×2、
   delattr 抛 TypeError、exception attributes）——unittest 断言机器
   （auto=24 的必然热点）被编译是共同前提。
2. 核心转储：崩点 `_PyFrame_Clear`（经 resumeInInterpreter 的 <030C
   入口帧清理）对 localsplus 减引用 → 某对象 dealloc →
   `PyObject_GC_UnTrack` 段错误；帧身份
   `_AssertRaisesContext.__exit__`，受害对象 = **traceback，
   refcnt==0 再 dealloc（二次释放）**。
3. deopt 清单：崩溃迭代 = `__exit__` 首次有机 deopt（'dict values
   check'，cause=226=语句边界、空值栈）；恢复后解释器正常执行到
   RETURN_VALUE（prev_instr=327），清理时爆。
4. 二进制内插桩（CI_DEOPT_SLOT_DEBUG，取证后已拆除）钉死账目：
   **pre-reify 时 tb 的 LiveValue 为借用（rk=kBorrowed）且指向
   refcnt=0 的尸体**——多减发生在 JIT 执行的前半段之前的引用结构，
   与 deopt/解释器后半段无关。
5. 账目还原：stock 3.11 `WITH_EXCEPT_START` 取得 traceback 后**立即
   归还新引用**（`PyException_GetTraceback; Py_XDECREF`），以借用
   指针放入实参窗口——解释器被调方入帧即 incref 实参，故 stock
   安全；JIT 被调方遵循"实参借用自调用方数组"约定全程不 incref。
   `_AssertRaisesContext.__exit__` 体内 `exc_value.with_traceback(
   None)` 剥掉 exc 持有的**最后一个** tb 引用 → 借用寄存器当场悬垂
   → 其后任意 deopt 物化对尸体 incref（复活）→ 帧清理 decref →
   二次 dealloc → GC_UnTrack 崩。

**修复 [P5]**（vendored ceval WITH_EXCEPT_START，D3 台账第五项，
哈希锁经 --generate 重签）：`Py_XDECREF(tb)` 移至 `__exit__` 调用
之后，引用持有跨越调用；用户可见语义不变。**定性**：这是"调用方
构造借用实参窗口 vs JIT 借用约定"这一普遍健全性缺口在 CPython
自身代码里最尖锐的暴露点（assertRaises 在测试语料中无处不在）；
定点封堵零每调用开销。**普遍解**（物化帧入口 incref 实参、镜像
stock 语义，代价为每调用 N 次引用操作）作为设计决策移交正式
M5/M6——本案证明借用约定的健全性依赖调用方行为，属 D9 家族的
调用协议侧。

**修复实证**：test_list / test_tuple / test_exceptions 全表面转绿；
基础 libtest 差分 5 发散 → 2（基线收缩入册）；diffgate 923 用例
（新增 [P5] 蒸馏用例 case_deopt_exit_strips_traceback）0 失败；
generators 语料基线原样；核心哈希门禁绿。

## 二、根因 B（残案立档）：test_builtin 全表面 GC 期崩溃

- 症状：仅完整 regrtest + 全表面编译时崩，faulthandler 停在
  "Garbage-collecting"（mock 创建触发 collect），即 GC 对象图早已
  被污染，崩点非毒源；`-m unittest` 路径 rc=1 不崩。
- 排除项（范围阀二分，全部不触发崩溃）：unittest 全目录、mock.py、
  test_builtin.py 自身、unittest+contextlib+re+enum、functools+
  inspect+typing+collections+traceback、test/support+libregrtest、
  os/io/tempfile/weakref/threading/warnings/ast——**非单文件毒源，
  组合/体量型**（与编译量相关的布局或 GC 时序条件）。
- 立档移交：ASAN 首批客户（毒源是内存写坏而非逻辑错，范围阀二分
  已到边际收益盡头）；复现配方
  `PYTHONJITAUTO=24 无范围阀 python -m test test_builtin`（确定性）。
- test_scope FAIL（非崩溃）与 libtest 配置③失败集合并入正式燃尽
  清单，未在本时间盒内追。

## 三、门禁与量化（P5 后）

- diffgate 主语料 918→923（M9R3 四例 + P5 一例）全绿；
- 基础 libtest 差分基线 5→2（cp311-libtest-basic.json 收缩）；
- libtest 配置③双阈值复跑：**t24 28/38 → 32/38**（descr /
  exceptions / inspect / tuple 四模块由 P5 回收）；t2 30/38（余集
  builtin/coroutines/inspect/scope/sys/sys_settrace/traceback/
  weakref，其中 test_sys.test_refcount 为总引用数计数断言差 2 的
  漂移敏感型，与 refleak 专项合并追踪）；双档共同余集 =
  builtin（根因 B）/coroutines/scope/sys/traceback/weakref，为
  正式燃尽首批；
- RuntimeTests / test_cinderx 首基线不受 P5 影响（P5 仅改 3.11
  vendored 循环；3.14 不编译该文件，无需反向验证——共享文件零
  触碰，git diff 为证）。

## 四、工时

约 2 小时（取证链 4 层递进：faulthandler→核心转储→deopt 清单→
二进制插桩；组合型残案的二分止损 ~30 分钟）。
