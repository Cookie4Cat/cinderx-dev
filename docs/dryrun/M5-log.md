# M5 预演记录：函数调用路径接管（引用计数断言）

> 起始：2026-07-04 ｜ 分支 dryrun/m5-calls ｜ 基线 dryrun-311-base（M4 三轮已合入 @6bafe0f00）
> M5 出口条件：调用矩阵（285+，含引用计数断言）全绿。差分层面 calls 285
> 用例在 M4 结束时已全部通过；本轮的增量即引用计数断言本身。

## 断言工具

`refcount_matrix.py`（docs/dryrun/ 存档；正式开发应并入 ci_pipeline/diffgate
工具集）：对每个语料用例，在 interp 与 jit 两种模式下测量 N=200 次重复调用
前后模块级对象的引用计数漂移，断言两模式漂移一致。合法的状态变更在两模式
中漂移相同；调用路径的引用计数缺陷表现为 jit 模式独有漂移。伪不朽单例
（True/False/None 等）排除在目标之外（其漂移属设计行为，见 M4-log）。

## 首跑结果与缺陷修复

首跑 calls 285 用例：interp 模式 0 项漂移，jit 模式 **183 项漂移**（被调用
函数每次调用净增 1 个引用）。特征二分（普通函数/闭包/函数属性/exec 生成
均无漂移）后锁定触发条件：**调用以异常退出**。五行最小复现确认：JIT 编译
函数每次抛异常泄漏 1 个函数引用；配合 gdb 断点计数确认异常退出路径完全
不执行 JITRT_UnlinkFrame。

**根因**：3.11 上入口帧的清理是调用方的责任（stock `_PyEval_Vector` 在
EvalFrame 返回后执行 `_PyEvalFrameClearAndPop`；eval 循环只弹出自己压入的
帧）。JIT 的异常退出经 deopt 路径进入 `resumeInInterpreter`，将帧交给
vendored 循环续跑后**缺少这一步调用方清理**——帧的函数引用、code 引用与
datastack 槽位在每次 deopt（含每次抛异常的调用）中泄漏。此前观察到的
偶发堆损坏（free(): invalid pointer）与 datastack 泄漏为同一来源。
3.14 不受影响（帧归属模型不同，清理由其他环节完成）。

**修复**：resumeInInterpreter 的 <0x030C 分支在求值返回后对
FRAME_OWNED_BY_THREAD 帧执行 `_PyFrame_Clear`（vendored 逐字版，含逃逸帧
所有权转移）+ `Cix_PyThreadState_PopFrame`，即 stock
_PyEvalFrameClearAndPop 的对应物。生成器帧由生成器对象负责清理，不在此
处理。备注：stock 版本还包含 recursion_remaining 的临时调整，预演未镜像
（仅影响深递归余量核算），正式 M6 复核。

## 终态

全部七个语料模块（918 用例）× 两模式：**除 ic_mutation 外全部零漂移**，
两模式漂移一致（M5 出口条件在预演口径下达成）。

**新发现（记档 M7）**：corpus_ic_mutation 在 jit 模式重复调用（N=200）下
段错误，复现率约 2/3（差分门禁每用例仅调用一次故未暴露）。与既知
"IC 对类变异不失效"家族同源，重复变异下由过期值升级为崩溃（推测 IC 使用
已释放的缓存条目，D9 级）。M7 版本号守卫工作的验收范围，建议配合 ASAN
排查。**重复调用维度应纳入差分门禁**（工具回流时作为 diffgate 的新模式）。

## 工时

约 2.5 小时（工具 40 分钟；183 项漂移的特征二分与根因定位 ~1.2 小时——
两轮假设被最小复现否定后由"异常退出"特征收敛；修复与全量复验 ~30 分钟）。

## 遗留（正式 M5/M6/M7 清单）

1. refcount_matrix 并入 diffgate 工具集，重复调用维度纳入门禁模式；
2. ic_mutation 重复调用段错误（M7，备 ASAN）；
3. recursion_remaining 调整的镜像复核（M6）；
4. 绑定方法免分配/builtin fastcall 专项微基准（M5 正式，预演未单测）。
