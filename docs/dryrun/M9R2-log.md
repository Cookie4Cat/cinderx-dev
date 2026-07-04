# M9 第二轮预演日志：真 auto 计数、CALL 特化回收与双阈值水位

日期：2026-07-04　分支：`dryrun/m9-perf-r2`　基线：`dryrun-311-base@bbc39419c`（M9 首轮合入后）

首轮遗留两问（用户提出）：① 自持解释器下能否实现 3.14 的真计数式
auto；② 特化字节码的类型信息能否接入。本轮双双落地，并以双阈值
（2 / 24）重测水位。

## 一、D3 补丁台账开张：vendored 循环首两个源级补丁

P1/P2 为宏级补丁（基座源逐字不动）；本轮引入首两个**源级**补丁
（宏无法改写结构体字段访问与 opcode 处理器内部语句），位点以
`[P3]`/`[P4]` 注释标记，哈希锁重签，台账记录于 cinderx_ceval.c：

- **[P3] start_frame 帧压栈计数**：3.11 无 code watcher，计数式 auto
  的每 code 计数改在解释循环帧压栈点完成——该点同时覆盖 vectorcall
  入口与特化 CALL 的内联压栈，是 3.11 上唯一数得全的位置（也正是
  M1 期 Ci_EvalFrame 处 TODO 注释的本意）。钩子实现于 pyjit.cpp：
  CodeExtra 由既有 `codeExtra()` 按需分配（3.14 经 code watcher 创建
  时分配的等价物）、阈值到达即编译、编译失败停用该 code 的 auto。
- **[P4] CALL 特化按被调方判定**：`CALL_PY_EXACT_ARGS` /
  `CALL_PY_WITH_DEFAULTS` 的 `DEOPT_IF(eval_frame)` 一刀切改为
  仅当被调方非解释器默认入口时 DEOPT——解释器间调用保留特化内联
  压栈，编译入口经通用路径进入 JIT。原 stock 检查服务于任意第三方
  求值器的 PEP 523 语义；本端口求值器即本循环自身，内联压栈语义
  等价。`Ci_StockEntry311` 空值时恒 DEOPT（fail-safe）。

配套：R1 的惰性 forced 安装机制撤除（单一机制原则）；3.11 不再做
启动期存量函数入口安装（存量函数经 [P3] 在执行时统一计数）。

## 二、机制验证

- **阈值语义**：PYTHONJITAUTO=5 实测第 5 次调用准时编译（gdb 钩子
  计数 55 次压栈）。注意：`python -c` 的代码 co_filename 为
  `<string>`，被合成文件名阀排除——冒烟须用脚本文件。
- **特化时序**：阈值 200 的 float 核实测编译出 `DoubleBinaryOp: 2`
  （类型化拆箱路径）——证实 R1 的"第二次调用即编译≈ALL 时序，编译
  的多为未特化字节码"，R1 水位系统性偏悲观。
- **解释器税**：richards 巨阈值（永不编译）实测 28.1ms vs stock
  22ms——P4 把 R1 时代 ~2 倍的求值器税压至 ~1.28x；残余主要为 [P3]
  钩子每压栈开销（配置读取+CodeExtra 查找），有明确打磨路径。
- **回归**：diffgate 940 用例 0 新增、反而修复 2 项（M8 基线的序列
  依赖 SIGILL 对未再现，与其定性一致，基线 6→4）；3.14 编译 0 错 +
  冒烟通过。

## 三、双阈值水位（同 R1 口径：p3/v5/w3，manylinux aarch64 容器）

| 基准 | R1（强制阈值2） | R2 t2 | R2 t24 |
|---|---|---|---|
| nbody | 1.235x | 1.195x | 0.948x |
| float | 0.884x | 0.882x | 0.998x |
| fannkuch | 0.989x | 0.995x | 0.971x |
| chaos | 0.537x | 0.667x | **0.979x** |
| regex_compile | 0.803x | 0.806x | 0.816x |
| nqueens | 0.910x | 0.764x | 0.784x |
| generators | 0.642x | 0.693x | 0.756x |
| richards | 0.517x | 0.646x | 0.701x |
| richards_super | 0.593x | 0.683x | 0.785x |
| go | **0.034x** | 0.659x | 0.719x |
| hexiom | 0.130x | 0.576x | SEGV |
| scimark | 0.067x | SEGV | 0.704x |
| sqlglot_v2_parse | TypeError | TypeError | 0.824x |
| sqlglot_v2_transpile | TypeError | TypeError | 0.794x |
| spectral_norm | 0.741x | SEGV | SEGV |
| deltablue / raytrace | 失败 | 失败 | 失败 |
| **几何均值** | **0.432x** | **0.762x** | **0.823x** |

## 四、判决修订

1. **R1 的病理层是机制税不是代码质量**：go 0.034→0.719（约 20 倍
   回收）、scimark/hexiom 同族恢复。R1"结构性差距"判决中病理部分
   撤销。
2. **当前形态真实水位 ≈ 0.76–0.82x**：数值类在 t24 已近持平
   （float 0.998 / chaos 0.979 / fannkuch 0.971），最差的 call/帧
   密集类 ~0.65–0.72（richards/go 族）。剩余差距与首轮归因一致
   （物化帧仪式 + helper 化 IC + ~28% 解释器残余税），但**量级回到
   打磨区间**——LWF 与 IC 内联快路径两大杠杆各有 10–20% 量级的
   回收预期，"性能可以慢慢优化"的路线恢复成立；LWF 翻转从"必须
   提前"降级为"高优先候选"。
3. **阈值敏感性证实设计书双档主张**：t24 整体优于 t2（特化时序
   收益，chaos 0.667→0.979 最典型）；反例 nbody（1.195→0.948）
   说明特化后编译并非全赢，正式 M9 需按基准逐项研究。
4. **正确性不稳定性是当前第一优先**：崩溃/失败集合随阈值漂移
   （spectral_norm 双档新崩、scimark 仅 t2 崩、hexiom 仅 t24 崩、
   sqlglot TypeError 仅 t2 现），与有机 deopt-resume 案（M9 首轮
   立案）的时序敏感定性一致。该案不修，任何正式性能结论都建立在
   流沙上。

## 五、移交（正式 M9/M6 范围）

有机 deopt-resume 修复（第一优先，崩溃集合随阈值漂移即其证据）、
sqlglot TypeError / raytrace exit 1 归因（差分语料新形态候选）、
[P3] 钩子打磨（配置缓存/早退排序）、CodeExtra 计数与 jitVectorcall
体系合流、nbody 特化时序反例研究、pickle_pure_python 架子补齐。

预演工时：约 2 小时。M9 两轮合计约 5.5 小时。
