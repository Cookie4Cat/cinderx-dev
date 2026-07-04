# M9 性能归因：为何当前整体弱于 stock 解释执行（richards 深挖）

日期：2026-07-04　环境：manylinux aarch64 容器（dryrun-perf，特权）
方法：旋钮分解矩阵 + gdb 批量采样（PMP，120 样本；Docker Desktop VM
的 perf 定时采样对持续负载失效，样本饿死，PMP 为可靠替代）+ 双侧
final HIR 指令谱 + 穿刺参照系复跑。

## 一、旋钮分解（richards worker，-l 16 -w 3 -n 8，均值 ms）

| 配置 | 用时 | 含义 |
|---|---|---|
| stock | 22.0 | 基线（发行版 PGO+LTO 解释器） |
| 加载 cinderx、不装求值器 | 21.1 | 加载本身零成本 |
| 装求值器、无 auto 计数 | 24.3 | **纯 vendored 循环质量税 +11%**（-O2 无 PGO/LTO） |
| 装求值器 + [P3] 计数、零编译 | 32.7 | **[P3] 钩子再 +38 个百分点**（解释执行合计 1.48x） |
| auto=2 全编译 | 34.0 | **JIT 稳态未跑赢被课税的解释器** |
| auto=2 + 无入口守卫 | 32.2 | 守卫包装税 ~5% |
| auto=24 | 30.1 | 特化时序收益，仍 1.37x |

稳态核查：**零 deopt、37 函数全编译、35.4ms/iter**——减速 100% 来自
JIT 生成码及其协议自身，与 deopt/解释器混跑无关。

## 二、PMP 时间去向（稳态，120 样本）

| 类别 | 占比 | 主要条目 |
|---|---|---|
| **属性/方法机器** | **~33%** | IC helper（LoadMethodCache::lookup、AttributeMutator）15% + libpython 泛型路径（_PyType_Lookup 5%、PyObject_GetAttr(+plt) 7%、GenericGetAttrWithDict 4%、AsUTF8 2%） |
| **帧/调用协议** | ~16% | JITRT 系、帧清零 __memset、excInjectFire（M6 注入检查点每 JITRT 调用都在跑） |
| **解释器循环残余** | ~12% | Ci_EvalFrameDefault_311 在稳态仍热：存在经解释入口 vectorcall 的高频调用（三个 1-2 参函数对象，callers=_PyObject_VectorcallTstate→_PyEval_Vector；身份待钉，疑 fn/谓词族） |
| libpython 其他/匿名 | ~31% | PyObject_IsTrue、调用胶水等 |
| **JIT 生成码本体** | **仅 ~8%** | DeviceTask.fn、Task.runTask 等 |

**一句话病理：JIT 码只占 8% 执行时间，92% 是围绕它的协议与 helper。**

## 三、穿刺参照系（同机、同解释器二进制、同基准）

| 形态 | richards 稳态 per-iter |
|---|---|
| stock | ~22 ms |
| **本端口（37 函数全编译）** | **35.5 ms（0.62x）** |
| **穿刺（38 函数 force_compile）** | **8.5 ms（2.6x 快于 stock）** |

- 穿刺"大幅优化"属实；本端口与穿刺差 **4.2x**。
- **结论性推断：3.11 平台上 JIT 大幅跑赢 stock 是已被证明可达的；
  当前差距是架构选型差异，不是平台上限。**
- 穿刺 pyperf 全流程跑不动（其 ⓪ 号异常路径 bug 在 auto 全表面立即
  发作，须 jitlist/force_compile 定向），当年冒烟数字即定向口径。

## 四、HIR 指令谱对照（同函数）

schedule：穿刺 507 行 HIR 含 **27 LoadField + 17 BitCast + 12
PrimitiveCompare（内联 IC 快路径展开）**；本端口 339 行含 **4
CallStatic（helper 化 IC）**。生成码尺寸本端口反而更小
（schedule 3176B vs 4912B）——小而慢的 helper 密集型 vs 大而快的
内联守卫型。isTaskHoldingOrWaiting 本端口已有内联 split-dict 形态
（simplify 路径），差距集中在 LOAD_METHOD/多态站点与调用协议。

## 五、远因定案（按时间占比排序）

1. **IC 架构（最大项，~33%）**：M7"3.11 不发射内联快路径、命中判定
   移入 fill helper"决策 + 单条目缓存对多态接收者（richards 的
   Task 子类族）持续 miss → 每次属性/方法访问 = C++ 调用 +
   泛型查找。对面 stock 是 PEP 659 内联特化，穿刺是内联守卫快路径
   （其性能层 d4380b72 当年因与 kunpeng IC 架构互斥被弃移植——
   该决策的性能代价现已量化）。
2. **帧+调用协议（~16%）**：每调用物化 _PyInterpreterFrame
   （分配+清零+链接）、JITRT_Call 间接层、excInjectFire 常开检查、
   入口守卫 ~5%。穿刺为 shadow-frame 时代轻帧设计。
3. **解释器残余（~12%）**：稳态仍有高频调用走解释入口（待钉身份）；
   叠加 vendored 循环 -O2 无 PGO（+11%）与 [P3] 钩子（对解释执行
   +38 个百分点）。
4. 打磨小项：excInjectFire 编译期门控、守卫包装 prologue 化、
   [P3] 钩子缓存。

## 六、优化路线建议（杠杆排序）

1. IC 内联快路径（含多态站点策略）——预期最大单项，穿刺已证可行；
2. 帧协议轻量化（LWF 翻转评估或物化帧瘦身：清零消除/延迟物化）；
3. 解释入口残余定位消除 + [P3]/excInject/守卫三项打磨；
4. vendored 循环 PGO/LTO（M2 构建配置对齐既定项，收 +11%）。

工时：约 2.5 小时（perf 失效改道 PMP 约占 40 分钟）。
