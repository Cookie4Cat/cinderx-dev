# M9 第十六轮：布局敏感崩溃家族销案（GC 遍历的幻影 LWF 帧头）

日期：2026-07-05　分支：`dryrun/m9-crash-hunt`　基线：持平冲刺轮
（旧口径 1.000；诚实口径 16 项在榜 + regex_compile/scimark/nqueens
三项 worker 确定性 SIGSEGV）

## 一、验尸链

ulimit -c 后按协议复现（run_ab --benches nqueens），核心转储直读：

```
#0 visit_decref(0xa00000000 / 0x210)      ← 垃圾指针
#1 jitgen_traverse (generators_rt.cpp)     ← GC 遍历 JIT 生成器
#6 _PyObject_GC_NewVar ← PyTuple_New ← 编译码执行中分配触发 GC
```

两次归因迭代（如实记录）：首轮误读行号归因给 spill 槽访问循环，
按"挂起态门"修复后崩溃原样——第二个核心转储配合 footer 直读
（`frame_header = {func = 0x210}`）实锤真凶为 **LWF 专属块**：

```cpp
#if PY_VERSION_HEX < 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    if (jit_gen->gi_frame_state < FRAME_CLEARED) {
      ... jitFrameGetFunction(frame) ...   // 读 LWF FrameHeader
      Py_VISIT(func.get());
    }
#endif
```

`jitFrameGetFunction` 在编译旗标开启时无条件走 LWF 帧头读取，而
3.11 运行时是 kNormal 物化帧、没有帧头——读到的是 GenDataFooter
邻居字节的垃圾。垃圾恰为 NULL 时 Py_VISIT 安全跳过，故随分配布局
显形——**"最小侵入即压制"的根源**（faulthandler/独立进程直跑
即变布局即不崩）。

## 二、修复与同族审计

真修一行：该块加运行时 `frame_mode == kLightweight` 门。同族全量
审计（M9R3 移交项"jitFrame* 头部辅助族 3.11 调用点审计"收官）：
jitgen_deopt 完成路径（M9R3 已修）、UnlinkFrame 的
increfFuncObjForNonGenerator、prepareForDeopt 的 DEOPT_PATCHED 检查
——其余三处均已有运行时门，**traverse 是最后一处漏网**。

加固两点保留（推理封堵，非观测崩因）：traverse 的 spill 访问加
FRAME_SUSPENDED 门（yieldPoint 恢复执行时不清，陈旧元数据对执行中
被复用的 spill 槽做 visit 属同型风险；执行态寄存器持有引用对 GC
不可见是安全方向——GC 将未解释引用计数视为外部根而保活）；
`JitGenObject::yieldFrom` 收紧到 FRAME_SUSPENDED（与 stock
_PyGen_yf 同门槛，原判据放过 EXECUTING 会对陈旧槽 INCREF）。

## 三、销案范围

- **regex_compile / scimark / nqueens worker SIGSEGV：销**（三项
  A/B 转绿：0.840 / 0.870 / 0.902）；
- **libtest test_builtin：崩溃销**（CRASH:11 → FAIL）。残余失败为
  vendored 3.11.6 锚点 vs 运行时 3.11.13/15 的上游微漂移
  （import_name 对非 dict builtins 映射的处理为 3.11 后续版本修复；
  正式目标 openEuler 锚定 3.11.6 时此漂移自然消失），已在 M2 微
  漂移基线内（libtest:test_builtin 条目），无需动作；
- 全表面编译（诚实口径）自此无已知崩溃面；libtest 唯一追踪项余
  test_scope（修复分支 dryrun/m9-nested-func-leak 待并）。

## 四、门禁

diffgate 923 全绿；refcount 矩阵六组零漂移；四套冒烟全过；3.14
反向编译 + 四项冒烟通过；libtest test_builtin 转 FAIL 落回既有
基线内、test_scope 单项在追踪。终态构建 = PGO 三相配方重跑。

## 五、A/B（诚实口径完整 19 项——本轮主数，存档
m9ch-ab-summary.json）

**几何均值 1.059，19/19 全绿——诚实口径（全表面编译）下全面越过
持平线。** richards 2.311 / richards_super 2.247 / deltablue 1.293 /
fannkuch 1.320 / nbody 1.228 / sqlglot 1.225 与 1.065 / chaos
1.024 / raytrace 1.001；前崩溃三项不止转绿：nqueens **0.962**
（旧口径 0.77——stdlib 编译反而是它的顺风）、regex_compile
0.912、scimark 0.906。余量组：go 0.669 / pickle 0.713 /
generators 0.828 / unpickle 0.830 / spectral_norm 0.887 /
hexiom 0.900 / float 0.984（结构判决见持平轮的编译价值分化表）。

三口径归档并列：旧口径（范围阀）1.000、诚实口径修复前 16 项
参考值 1.083、**诚实口径完整 19 项 1.059（正式口径）**。速通
战役至此：0.432 → 1.059。

## 六、方法论沉淀

- 布局敏感崩溃的取证要点：**核心转储 + 崩溃帧参数直读**（footer
  整体 print 一眼看穿幻影头槽），比脚本化断点快；两次归因迭代的
  教训——行号漂移下先 `sed` 对准当前源码再下结论；
- 编译旗标（ENABLE_*）与运行时模式（frame_mode）是两层门，**凡
  旗标门内读版本特定内存布局的代码必须再加运行时门**——本案与
  M9R3 幻影头槽案同根不同点，审计清单须以"读布局的辅助函数"为
  索引全量走查，而非逐崩溃逐修。
