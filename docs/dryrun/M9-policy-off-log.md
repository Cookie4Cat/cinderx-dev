# M9 运行配置轮：自适应冻结层默认全关与 values 分支化终审落地

日期：2026-07-05　分支：`dryrun/m9-policy-off`　基线：probation 轮
（几何均值 0.863x）

## 一、决策：策略层让位于基础优化（用户拍板）

自适应冻结（ROI backoff / IC 压力密度 / 计时试用）默认全部关闭，
knob 保留（CINDERX_AUTOJIT_ROI_BACKOFF / _IC_PRESSURE_RATIO /
_PROBATION）。理由：

1. **冻结把病理藏出剖析视野**——witness 轮取证成本（"码在 map、
   零 deopt、全解释"三谜面）的直接来源；
2. **冻结时机方差污染基准**——A/B 单项 ±0.15 方差带的主要成分，
   逼出"可疑项必复测"的流程税；
3. **过滤不解决根因**——基础优化时代需要的是病灶全暴露，策略层
   应是收尾阶段按需启用的最后一层。

## 二、观测红利立现：裸基线暴露两处被掩盖的病理

关闭后进程内裸基线：richards 15.47 / deltablue 1.82 / unpickle
2.99（均健康——witness 修复不依赖冻结），**go 179.3ms、raytrace
183.7ms**——两者此前的"可接受"数字全部来自冻结均衡的化妆。

## 三、values 守卫分支化三审终落地

前两审的否决理由在无策略体制下全部蒸发：① laggards 轮输给的
"风暴→backoff 冻结"均衡已不存在（对手从 91ms 变成 179ms）；
② probation 轮担心的"密度信号随行内直读消失"已无对象（密度层
默认关）。落地后 **go 零 deopt**（gdb 直方图空），进程内 go
179.3→167.6（-7%）、raytrace 183.7→177.3（-3.5%）、deltablue
1.74ms（历史最佳）；richards 15.23 持平。

至此基准套件**无任何 deopt 风暴**：richards 族由 witness 站点
见证门根治，go/raytrace 族由分支化根治——冻结层在本套件上已无
可捕之物，其保留价值仅为未知负载形状的保险。

## 四、当前运行配置（本轮后的默认形态）

| 层 | 默认 | 说明 |
|---|---|---|
| PYTHONJITAUTO=N | 协议用 2 | P3 帧压栈计数触发编译 |
| ROI backoff | **关** | deopt 预算冻结（opt-in） |
| IC 压力密度冻结 | **关** | stub 慢尾 stadd 计数保留在码内但 ratio=0 不发射 |
| 计时试用 | 关 | 研究旋钮 |
| 入口守卫包装 | 开 | 递归检查 + tracing pause（正确性口径） |

go 残余的 167ms（vs stock 61ms）现在是纯粹的基础优化标的：
DescrOrClassVar helper 密度、调用/帧协议税、字典操作——与
generators 轮同源，无一被过滤。

## 五、门禁

diffgate 923 全绿；generators 语料/基础 libtest 差分基线原样；
refcount 矩阵五组零漂移；两轮冒烟复跑；3.14 反向编译（ninja）+
attr/method/store/module 冒烟通过。

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档 m9off-ab-summary.json）：
几何均值 0.863→0.845——**回吐的 ~2pp 全部是冻结均衡的化妆**（go
0.567→0.360、raytrace 0.822→0.755 现为真实数），同时 deltablue
0.808→0.914（分支化红利）、richards 1.681；其余带内。自此 A/B 的
每个数字都是无过滤的真实编译态水位，方差带亦随冻结时机竞速消失而
收窄可期。
