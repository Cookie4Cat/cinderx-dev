# M9 第十二轮：speculative inlining / LWF 拍板件

日期：2026-07-05　分支：`dryrun/m9-inline-verdict`　基线：帧仪式行内
化轮（几何均值 0.933x；richards 1.944 / deltablue 1.107）

## ○、待决问题

从 0.93 向 1.0+ 走的两条候选大路：① LWF（轻量帧）3.11 移植（v1.1
独立件，M9R3 起挂账）；② speculative 方法内联（归因轮标定的
20-25% 理论池）。二者纠缠于同一门控：HIR 内联器被
`frame_mode != kLightweight` 强关（"内联帧无法安全解链"）。本轮
以实测为两条路定价。

## 一、证据一：LWF 的边际价值已坍缩（3.14 同机对照）

dryrun-m314 容器，同 benchmark 驱动（PYTHONJITALL=1）：

| 3.14 配置 | richards | deltablue | raytrace |
|---|---|---|---|
| stock | 21.32ms | 1.21ms | 116.2ms |
| JIT LWF（内联器关） | **12.21** | **1.24** | 116.2 |
| JIT normal 帧 | 17.40 | 1.65 | 143.7 |

LWF 对 normal 的红利（-30%/-25%/-19%）真实存在——**但 3.14 的
normal 帧仍走两次出线 C helper 仪式**，恰是上轮在 3.11 已行内化
收掉的税。横向对照：3.11（帧仪式行内化后）richards 11.34ms 已优于
3.14 LWF 的 12.21ms；相对各自 stock 的比值 1.99x vs 1.75x。LWF
残余边际 = 惰性物化（免字段初始化）+ 按需重建，估计上限 5-10%，
而移植成本 = 布局感知重做（init 即 SEGV 的已知根因）+ 3.11 reifier
+ 全部自省/traceback 面 + 门禁体系重验。

## 二、证据二：现版内联器本身是净负收益（跨版本互证）

- **3.14 LWF 上**（内联器被支持的原生环境）：内联器开 vs 关 =
  richards 13.31 vs 12.21（**-9%**）、deltablue 1.29 vs 1.24、
  raytrace 117.1 vs 116.2——全线负。
- **3.11 端到端实验**（见证据三）：纯乐观路径微基准，内联版
  71.0ms vs 非内联 59.4ms（**+20% 劣化**）。非内联路径经过 IC
  内联/帧仪式行内化后，一次编译态小函数调用已足够便宜；现版内联
  器的产物质量（成本模型、内联后体的优化质量）反而更差。

**"解锁内联器"的前提不成立**：内联质量是先决工作流，与帧模式
无关；LWF 不是内联的门票。

## 三、证据三：3.11 普通帧内联的缺口清单（实验实测）

实验件（默认关，仅研究口径）：
1. `CI_JIT_INLINER=1` 在 normal 模式强开内联器（pyjit.cpp）；
2. **守卫式函数常量化前门**（builder.cpp，仅内联器开启时发射）：
   3.11 的 LOAD_GLOBAL 守卫式装载只保证"当前值"，内联器要求
   `hasValueSpec(TFunc)`——编译期窥得函数对象时追加 `GuardIs`
   同一性守卫（镜像 3.14 分支形态），全局重绑定即 deopt。

实测结果：
- **点火成功**：`Inlining function callee into caller`，乐观路径
  1000 轮正确、万次调用引用计数零漂移；
- **异常路径断裂**（普通帧内联的真缺口）：内联体内 raise →
  kRaise deopt → **多帧重建 3.11 未完成**——traceback 出现两个
  伪 `<module>` 帧、被调帧缺失，恢复后调用方栈槽污染
  （`'int' object is not callable`）。M9R3 帧重建家族的内联版，
  DeoptMetadata 的 frame_meta 内联栈结构存在，3.11 的 reify/
  resume 路径未接多帧；
- **自省语义缺失**：kNormal 下 BeginInlinedFunction/
  EndInlinedFunction 是空操作（内联体不建帧），
  sys._getframe/traceback 在内联体内无正确帧可见。

## 四、判决建议

1. **LWF-3.11 移植：不做**（维持 v1 范围外）。边际收益坍缩
   （证据一），成本不成比例；帧税已由普通帧的编译期常量折叠路线
   收割，且该路线在 3.11 上的相对成绩已超过 3.14 LWF。
2. **内联轴：暂缓，先决条件重定义**。现版内联器跨版本净负
   （证据二），speculative 方法内联在其上叠加只会放大负值。正确
   顺序：内联器产物质量工作流（成本模型/内联后优化）→ 多帧
   deopt 重建（正式 M6 的自然扩展）→ 内联体建帧或 reifier 语义
   → 再谈 speculative。
3. **保留资产**（本轮入库，默认全关零影响）：守卫式函数常量化
   前门（speculative 的第一块砖，通用件）、CI_JIT_INLINER 实验
   开关、缺口清单与确定性复现（/tmp/inline_stress.py 形态）。
4. **性能余量的现实主义重排**：go/pickle/regex 残余的下一优先
   杠杆是 vendored 循环 PGO/LTO（实测 +11% 解释残余税，纯构建
   配置）与调用协议深水位，而非内联。

## 五、门禁

默认路径零变更口径（全部新代码在默认关的双闸后：GuardIs 仅
`hir_opts.inliner` 开启时发射，而 normal 模式下内联器仅
`CI_JIT_INLINER=1` 可开）。diffgate 923 全绿；refcount 矩阵六组
零漂移；三套冒烟全过；3.14 反向编译 + 四项冒烟通过；libtest 差分
两项 DIVERGE 与前两轮一致（test_scope / test_builtin 既存跟踪项），
无新增。进程内性能与上轮持平（richards 11.46 / deltablue 1.40 /
raytrace 153.4）。

## 六、A/B（19 基准，回归确认口径，存档 m9iv-ab-summary.json）

几何均值 **0.933，与上轮精确持平**（richards 1.991 / deltablue
1.111 / raytrace 0.862 / go 0.618 / generators 0.718，波动均在带
内）——实验件默认全关的零影响口径成立。
