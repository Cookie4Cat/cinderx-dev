# M9 第十五轮：持平冲刺（范围阀解除 + P3 瘦身 + 编译价值分化的发现）

日期：2026-07-05　分支：`dryrun/m9-parity`　基线：入口守卫轮
（几何均值 0.989x）

## 一、范围阀解除（测量口径修正）

run_ab.py 的 B 侧自 M9 首轮起带 `CI_JIT_AUTO_ONLY_PREFIX`（历史
动因：有机 deopt-resume 未决崩溃——已于 M9R3 六根因修复；全表面
libtest 仅剩两项已知跟踪项）。本轮起 B 侧默认全表面编译（诚实
口径），`AB_LEGACY_PREFIX=1` 保留旧口径以对照历史存档。

预期修正之一被证伪：pickle 的热脊柱 `_Unpickler.load` 在无阀下
仍不可编译——两道串联拒编门（"try 内回跳循环且 handler 返回"
穿刺整取阀 + "无可达正常返回" M4R3 阀；load 的唯一 return 在异常
handler 内，3.11 异常模型下 handler 不进编译码，结构性触发）。
已立独立专项（两道门的解法设计随任务移交）。

## 二、P3 钩子瘦身

`Ci_AutoJitCountFramePush311`（每解释帧压栈执行）三件瘦身：
co_extra 行内直读（共享镜像结构入 code_extra.h，与入口分派缓存
去重）替代 PyUnstable_Code_GetExtra 出线调用；"已决"快速返回
（已禁用/计数已过阈的 code 不再进入前缀比对与全套检查——永不热
的代码此前每次压栈全额付税）；检查顺序重排。

**排障实录（红线级）**：首版把 `getConfig().compile_after_n_calls`
做成函数级 static 缓存——本钩子在 cinderx.init() 旗标处理完成前
即随首批解释帧执行，static 把未初始化的 nullopt 焊死，auto-JIT
整体失效（richards 全员解释）。**红线：被解释器每帧调用的钩子不
可对 init 期可变配置做 static 缓存**。getConfig() 是全局结构直读，
本就不是成本中心；真正的成本在 co_extra 出线调用与前缀字符串比对
（已由前两件消除）。

## 三、意外的战略发现：编译价值按形态分化

auto 失效事故意外给出了本机的**纯解释基线**（P4/P3 瘦身/帧行内化
后的 vendored 解释器），修复后补齐双模对照（同一二进制）：

| 基准 | JIT（auto=2） | 纯解释（巨阈值） | stock | 判决 |
|---|---|---|---|---|
| richards | 10.21ms | 22.06 | 22.59 | 编译 2.2x 碾压 |
| deltablue | 1.36ms | — | 1.57 | 编译胜 |
| go | 96.3ms | **72.1** | 61.5 | **解释胜 25%** |
| unpickle | 5.22ms | **4.94** | 4.13 | 解释小胜 |
| generators | 27.10ms | 26.89 | 18.9 | 打平 |

vendored 解释器经 P4（特化调用内联压栈保留）+ P3 瘦身 + PGO 后已
接近 stock 水平（go 解释比值 0.89 vs stock），而 go/pickle 形态的
编译产物仍败于 stock 的 PEP 659 行内特化——**这些基准的 A/B 落后
主要在为"强制编译"买单**。基础优化十四轮后，该分化是结构性的：
补齐需要"选择性编译"（自适应策略层，用户此前决策默认关、留收尾）
或"编译产物在这些形态上打赢行内特化"（typed-dict/派发特化级工作，
超出 dry-run 范围）。此为收尾阶段重启策略层讨论的决定性数据。

## 四、门禁

diffgate 923 全绿；refcount 矩阵六组零漂移；四套冒烟全过；3.14
反向编译 + 四项冒烟通过；libtest 两项既存 DIVERGE 无新增。终态
构建 = PGO 三相配方重跑（161 gcda）。

## 五、A/B（双口径，存档 m9par-ab-summary.json /
m9parL-ab-summary.json / m9par-ab-rerun-summary.json）

**旧口径（范围阀，与历史存档同轴）：19/19 全绿，几何均值
0.989 → 1.000——持平达成**（richards 2.323 / richards_super 2.273 /
deltablue 1.308 / chaos 0.990 / raytrace ~0.99；落后组 go 0.665 /
pickle 0.648 / unpickle 0.727 结构未变——见三的编译价值分化）。
增量归因：P3 瘦身 + 本轮构建漂移 ≈ +1.1pp。

**诚实口径（全表面，本轮引入并设为默认）：16 项在榜**，其中
sqlglot_v2_parse **0.834 → 1.247**、sqlglot_v2_transpile
**0.808 → 1.071**、generators 0.843、unpickle 0.803、raytrace
0.994、chaos 1.013——stdlib/三方库热代码进 JIT 后大类回收；
**regex_compile / scimark / nqueens 三项 B 侧 worker 确定性
SIGSEGV**（协议内 3/3 复败；同负载独立进程直跑通过——"最小侵入
即压制"的布局敏感潜伏家族，与既档 test_builtin 案同族）。已立
独立专项（协议内确定性复现配方随任务移交），修复后诚实口径方可
给出 19 项几何均值。16 项口径的几何均值 1.083 仅作参考（缺席三项
不可比）。

## 六、遗留

- pickle 脊柱两道拒编门专项（已立）；
- 选择性编译决策（策略层重启讨论，数据见三）；
- 巨阈值解释对照中 richards 22.06 与旧 P3 时代 ~29 的差距未逐项
  归因（P3 瘦身 + 多轮解释器侧变更叠加）。
