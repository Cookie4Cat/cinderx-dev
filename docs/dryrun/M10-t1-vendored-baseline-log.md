# M10 第五轮：T1 vendored 本底税专项（构建因素矩阵）

日期：2026-07-07　分支：`dryrun/m10-c3-stub` 续　基线：C3 轮
（纯 LTO 交付口径）

## 一、对象与方法

T1 = auto=0 判别口径（vendored 循环 + 近零钩子）下相对 stock 的
本底差，归因轮实测 8~24%（调用/递归密集形态最重）。本轮以单因素
实验矩阵逐项定价：六项代表集（deepcopy/pickle/pprint/go/unpickle/
richards），每实验独立构建独立测量，杜绝合并归因。

## 二、构建因素审计

- **计算 goto 派发：在位**（pyconfig.h 有 HAVE_COMPUTED_GOTOS，
  反汇编 Ci_EvalFrameDefault_311 计 199 个间接跳转位点，排除）；
- **优化档位：命中**——项目默认 -O2 -g -DNDEBUG，而 stock CPython
  的 ceval 为 -O3（configure 默认 OPT），巨型派发循环正是两档差异
  最大的形态；
- **Bsymbolic-functions：已在**（链接选项在位，.so 内部自调用无
  PLT 跳板——3.14 主仓同款课已抄）；
- 钩子调用位点与 -fno-plt 列为实验项。

## 三、实验矩阵（auto=0，六项几何均值）

| 实验 | 几何 | 判决 |
|---|---|---|
| 基线（纯 LTO，interpreter -O2） | 0.816 | — |
| E1：interpreter 目标 -O3 | **0.834（+1.8pp）** | **落地**（deepcopy +4.8、pprint +2.6、richards 解释态 +2.2，零回退） |
| E2：E1 + P3 钩子位点编译期移除 | 0.839（+0.5pp） | 不落地——LTO 已内联早退大半，位点内联化收益不抵复杂度；测量护栏（CI_T1_NO_AUTOJIT_HOOK）留档 |
| E3：E1 + -fno-plt | 0.827（−0.7pp） | 弃（净负） |

## 四、残余定价与判决

E1 落地后 auto=0 残差仍 ~17%（六项几何 0.834）。构成判定：

- **主体 = stock 的 --enable-optimizations PGO 优势**——CPython
  官方口径 PGO 对解释器为 +10~15%，与残差量级吻合；我方对位项
  被 PGO-use 相概率性错译（C3 轮案三）阻塞，**T1 的主解 = PGO
  工具链专项收口后恢复 PGO 交付口径**；
- 次要 = 跨 DSO 结构固有差（vendored 循环在 .so、stock 静态单体，
  外部 C-API 调用的 PLT/可见性差已被 Bsymbolic 覆盖到内部件，
  外部件 -fno-plt 实测不赚）；
- P4/P5 补丁残差未单测（预期 <1pp，随 PGO 恢复一并复测）。

**判决：T1 以 -O3 落地收 +1.8pp（解释态口径），其余体量押在 PGO
专项；钩子链内联（T2）按 +0.5pp 上限降级至机会项。**

## 五、门禁与量化

JIT 态（auto=2，纯 LTO + interpreter -O3，存档
m10t1-ab-summary.json）：解释器份额高的项小幅受益（go 0.668
[+1.5pp]、docutils 0.760 [+0.6]），编译主导项持平；守护组零回退
（richards 2.168 / richards_super 2.124 / deltablue 1.262 /
raytrace 0.964）。-O3 的 +1.8pp 为解释态口径，JIT 态按解释器份额
稀释——纯解释负载（短命进程、tracing 回退场景）全额受益。

门禁（纯 LTO + interpreter -O3）：冒烟七件全过；diffgate 923 全绿；
refcount 六组零漂移；libtest 相对基线无新增分歧；3.14 反向编译 +
两 smoke 通过（-O3 对 3.14 interpreter 目标同样生效且无害）。
