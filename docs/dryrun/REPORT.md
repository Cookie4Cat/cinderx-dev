# CPython 3.11 适配预演总报告（M0–M9 速通 + 性能十六轮）

> 时间：2026-07-03 晚 — 2026-07-05 ｜ 基线：kunpeng 仓 dev @75f0f552
> 性质：工程预演（dry-run）。出口 = 基本功能可运行 + 坑清单 + 实测
> 工时 + 可复用资产；预演代码不直接进入正式开发，按产出物白名单移交。
> 分支轨迹：dryrun/m0-gates … dryrun/m9-crash-hunt，MR #2–#34 全部合入。

## 一、执行摘要

**预演超额完成。** 原定出口为"冒烟级可运行"；实际达成：

- **性能：19 基准 pyperformance A/B 几何均值 1.059（诚实口径：全表面
  auto=2 编译、无范围阀、19/19 全绿），全面越过 stock 持平线。**
  战役轨迹 0.432 → 1.059（首轮可比口径起算）。单项：richards 2.311、
  richards_super 2.247、fannkuch 1.320、deltablue 1.293、nbody 1.228、
  sqlglot 1.225/1.065、chaos 1.024、raytrace 1.001。
- **正确性：全表面编译无已知崩溃面。** diffgate 923 用例全绿（穿刺
  时代 75 失败起步）；refcount 矩阵六组零漂移；libtest 差分唯余
  test_scope 一项在追踪（修复分支现成待并）与 test_builtin 微漂移
  （锚点差异，正式目标自然消失）。
- **门禁体系、构建配方、方法论红线全部沉淀为可移交资产。**

## 二、里程碑编年（含实测工时）

| 里程碑 | 交付 | 工时 |
|---|---|---|
| M0 | 差分门禁框架 + 918 语料 + libtest 差分 + opcode 覆盖工具 | 预备夜 |
| M1 | 3.11 可构建可导入 + JIT 拒编阀 + 哈希/SRPM 门禁；D7 判决 vendor-from-upstream | — |
| M2 | 逐字 vendor ceval/specialize/frame（哈希锁）；libtest 25/26 直接等价 | ~3.5h |
| M3 | LWF 编译期默认无版本意识 SEGV 定案（D4）；逃逸帧 take_ownership | ~2.5h |
| M4×3 | 语料 118→9：替身 ABI 错位、csel 立即数、单例不朽性烧穿等家族歼灭 | ~7.5h |
| M5 | 异常路径帧泄漏（调用方清帧责任）修复；refcount 矩阵工具 | ~2.5h |
| M6 | 递归守卫包装、D8 tracing 预演、异常注入 fuzz 基建 | ~2.5h |
| M7 | D5 拉式版本守卫（watcher 空桩案）；diffgate 首次全绿 | ~1.5h |
| M8 | D6 生成器；gi_code/STACK_CLEAR 引用会计双缺陷互掩案 | ~3h |
| M9 基建 | auto-JIT 接线（[P3]/[P4]）、有机 deopt 六根因、测试套件接入、全表面 SEGV 四案（[P5]） | ~13h |
| 性能十六轮 | 见下表 | ~45h |

## 三、性能战役编年（几何均值轨迹）

| 轮 | 内容 | A/B |
|---|---|---|
| 首轮水位 | 结构性差距定案 | 0.432 |
| R2/R3 | [P3][P4] 真 auto + 六根因修复，19/19 首次全绿 | 0.729 |
| IC 内联 | SplitMutator 真实现 + attr/method 内联 stub | 0.794 |
| 见证门控 | TypeExact 守卫风暴根治（richards 首破 1.0） | 0.808 |
| 计数矩阵 | 常驻 IC 计数基建 + method stub 死代码案 + store 侧 | 0.838 |
| 落后组 | 天生物化 hint 化 + 共享键成长驱逐 | 0.858 |
| IsTruthy | TBool 行内快路径 | 0.859 |
| go 三件套 | DescrOrClassVar hint + store 内联 stub（容量越界/别名案） | 0.874 |
| 策略层试验与关闭 | 密度冻结实现后按用户决策默认全关，诚实基线 | 0.863→0.845 |
| go 基础 | kind 直方图归因修正：终身 kSplitInline 案 + lm 楔死 | 0.865 |
| 帧协议轴 | send 链压层 + 入口每调用哈希消解 | 0.892 |
| 帧仪式行内化 | 建帧/拆帧编译期常量折叠（deltablue 首超 stock） | 0.933 |
| inline/LWF 拍板 | 三证据判决：LWF 不移植、内联暂缓（实验件入库） | 0.933 |
| PGO/LTO | 三相配方（编译器自身提速计入短窗） | 0.964 |
| 入口守卫消解 | 递归/tracing 下沉序言（零失败簿记账本） | 0.989 |
| 持平冲刺 | P3 瘦身 + 诚实口径切换 | 旧轴 1.000 |
| 崩溃销案 | 幻影 LWF 帧头 GC 遍历案 + 家族审计收官 | **1.059** |

## 四、可移交技术资产

1. **vendored 解释器与补丁台账 [P1]–[P5]**（哈希锁 20 文件）：
   影子版本发号器、auto-JIT 帧压栈计数（已瘦身）、CALL 特化按被调
   方判定、WITH_EXCEPT_START 借用窗口封堵。
2. **拉式 IC 体系**（3.11 无 watcher 的完整替代）：条目级
   tp_version_tag 三重校验、attr/method/store 三套 aarch64 内联
   stub（kind 2/3 双收、物化 me_key 自验证 hint、split 包装容量守
   卫）、模块/类属性站点扩展、共享键成长驱逐、kind 直方图诊断计数
   器（PYTHONJITCOLLECTINLINECACHESTATS）。
3. **帧与调用协议**：普通帧建帧/拆帧编译期常量折叠（FrameInitPlan
   双表）、入口守卫序言化（预检不落账 + 建帧扣减 + 三点补账 + OSR
   对冲，CodeRuntime 旗标三方同源）、入口分派 jit_compiled 元组缓
   存、gen send 链压层。
4. **构建配方**：PGO+LTO 三相脚本
   ci_pipeline/scripts/build_pgo_lto_311.sh（A/B 实测 +3.1pp）；
   CMake 接线本已完备仅需操作化。
5. **门禁体系**：diffgate 923 语料（三模式差分）、refcount 矩阵
   （六组、确定化判据）、基础 libtest 差分（微漂移基线）、配置③
   双阈值 libtest、RuntimeTests/test_cinderx 接入、四套冒烟（含
   entry_guard 语义冒烟、store 同值覆写引用平衡）、3.14 反向编译
   双绿闸、异常注入 fuzz（CI_EXC_INJECT）。
6. **测量工具**：run_ab.py（诚实口径 + AB_LEGACY_PREFIX 历史轴）、
   attach 循环 PMP 配方、双模对照法（编译 vs 巨阈值纯解释）。
7. **实验开关（默认全关）**：CI_JIT_INLINER + GuardIs 函数常量化
   前门、CI_JIT_NO_ENTRY_GUARD、自适应策略层三旋钮（ROI/密度/试用）。

## 五、方法论红线清单（正式开发必读）

1. 跨版本引用会计必须成对审计"谁持强引用"（M8 双缺陷互掩案）。
2. builder 内联守卫的 FrameState 必须等于指令边界前状态（sqlglot 案）。
3. 守卫用例必须逐轴变异，不只同类型改版本轴（raytrace 错派发案）。
4. 行内化 C++ 参照代码时，读-改-写跨越其它对象访问者，别名场景必须重读（store stub 案）。
5. **镜像 C 结构体字段的行内 asm 必须 static_assert 字段宽度**（use_tracing uint8 案：padding 垃圾按分配布局随机显形）。
6. **编译旗标门内读版本特定内存布局的代码必须再加运行时模式门**；审计以"读布局的辅助函数"为索引全量走查（幻影 LWF 帧头家族）。
7. 被解释器每帧调用的钩子不可对 init 期可变配置做 static 缓存（P3 案：auto-JIT 整体静默失效）。
8. 新调用形 LIR 指令五点接线清单，postalloc 操作码保留条件为高危遗漏位（method stub 死代码案）。
9. LIR 无条件跳转终结的块之后放置纯分支目标块用 switchBlock 而非 appendBlock。
10. macOS 绑定挂载陈旧构建陷阱：每次构建前后 md5 确证；docker cp 保留宿主 mtime 使 make 不重编。
11. 换库前必杀跑动中的 A/B；A/B 单跑可疑项必复测；受控进程内稳态为单项判据、A/B 为套件级回归检查。
12. 自适应策略评审基线必须同日同构建（跨构建布局漂移 ±6%）。
13. 跨会话禁止共享工作树（checkout 直通容器构建输入）。
14. gdb 批处理脚本 continue 前必须 run；-O2 下函数断点可能永不解析，归因用 C++ 计数器直方图。
15. 布局敏感崩溃取证："最小侵入即压制"提示布局依赖，核心转储+崩溃帧参数直读优先；行号漂移先对准当前源码再下结论。

## 六、风险与未决（正式开发输入）

**未决问题（均有档案/复现/专项）：**
- test_scope 实例泄漏：根因已明（M9R3 CF 强引用有根链），修复分支
  dryrun/m9-nested-func-leak 验证过待并；
- 编译态递归配额减半异常（千次异常后可达深度减半）：确定性复现
  脚本在案，专项已立；
- pickle 分发脊柱两道拒编门（try-loop-handler 阀 + 无正常返回阀）：
  专项已立，解法方向随案移交；
- 挂起帧 f_locals 空（任意点 localsplus 重建）：正式 M6 主体；
- descr 自身类型二重版本、refleak gen 专项、ASAN 全量未跑；
- **PGO-use 相概率性红态（已收口，判决翻案）**：原判"GCC-14
  PGO×LTO 工具链错译"撤销。根因为移植层缺陷：deopt 垫片对
  resumeInInterpreter 第四实参（is_instrumentation_deopt）的装配
  被 3.12 版本门跳过，3.11 下该 bool 实为 prepareForDeopt 返回后的
  未定义寄存器残值——残值非零即错入 instrumentation 恢复语义，
  kRaise 不再重执行，异常凭空丢失。"概率性/构建相关"的表象来自
  各构建寄存器分配骰子（plain/纯 LTO 侥幸残 0）。已修复
  （generator.cpp 版本门移除，两架构同修）并经红态 gcda 原位重建
  判决绿转；相四产物验收步保留为构建体系常设防线（探针剔除
  test_builtin 基线已知项）。详见 M10-pgo-rootcause-log.md。
  **PGO 交付口径恢复的工具链障碍不复存在**。

**结构性判断（已有决定性数据）：**
- **编译价值按形态分化**（M10 sqla 净效应轮修订）：richards 编译
  2.3x 碾压；曾长期为负的 go/sqlalchemy 编译净效应实为 IC 慢路径
  线性键扫缺陷（getDictKeysIndex O(n)，已修为哈希探测），修复后
  go 1.046 / sqlalchemy_declarative 1.064 反超 stock；余量组
  （pickle 0.79/deepcopy 0.73/generators）主体为短命进程 T1 税与
  钩子税，补齐路径 = 选择性编译（策略层三旋钮现成，默认关待决策）
  或编译产物赢过 PEP 659 行内特化（typed 级，超预演范围）；
- **LWF 不移植**（3.11 帧税已由常量折叠路线收割，相对成绩已超
  3.14 LWF）；**现版内联器净负**（跨版本互证），内联轴先决顺序 =
  产物质量 → 多帧 deopt 重建 → 内联体帧语义 → speculative；
- **早产编译与阈值三难**（spectral 验尸轮）：auto=2 在 quickening
  （第 8 次调用）前编译，HIR 恒读生字节码；阈值提至 16 可收数值类
  型情报（spectral 类 +21pp）但少调次大函数类（nbody 的 advance）
  因覆盖损失回退——**完整解 = 阈值提升 × OSR 环内接管联动**（后备：
  成熟度感知二次编译），交付阈值维持 2；属性/方法单观测精确类型
  投机已定性为多态陷阱并默认关（specialized_attr_speculation）。

**环境差异风险（manylinux 预演 → openEuler 正式）：**
- borrow 44 符号为静态 libpython 悲观上界，openEuler --enable-shared
  预计近零，须目标容器复测；
- TLS tstate 偏移探测在剥符号静态 Python 上被禁用（预演用烘焙
  gilstate 地址），openEuler 形态待验；
- test_builtin 微漂移随 3.11.6 锚点对齐自然消失；
- PGO 训练集已定稿为 16 基准多形态混合（train_full_311.sh，面板
  几何 +2.9pp 零回退）；解释器纯本底对训练内容不敏感（auto=0 口径
  0.857→0.858），T1 残差为结构性差异而非画像饥饿，勿再投训练侧；
- 本报告全部性能数字为单机（Apple Silicon Docker aarch64）p3v5
  预演严谨度，正式结论需目标机完整协议复测。

## 七、建议的正式开发顺序

1. 按里程碑设计书 M0–M9 展开，预演资产按白名单取用（门禁先行）；
2. 崩溃/泄漏类档案（第六节）在对应里程碑内优先销账；
3. 性能层按本战役定型顺序移植：拉式 IC → 帧仪式行内化 → 入口守卫
   序言化 → PGO/LTO 配方 → 按目标机重新归因再取余量；
4. 选择性编译决策在性能基线稳定后以双模对照数据重启。
