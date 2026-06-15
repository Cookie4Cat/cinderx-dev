# AutoJIT 用例函数形状与策略判断证据表

## 1 文档说明

本文档独立维护 AutoJIT 行为分类策略的用例级证据表。它不替代需求、功能设计或详细设计；它回答一个更具体的问题：**某个 pyperformance 用例里，CinderX JIT 实际编译了哪些函数，这些函数落在什么结构形状上，当前策略应该放行、延迟还是继续收窄。**

后续调策略时先读这张表。任何新增样本必须按同一口径补充：用例、测试口径、数据来源、函数列表、函数形状、当前策略、策略判断。

## 2 修订记录

| 版本 | 日期 | 修订人 | 修订说明 |
|---|---|---|---|
| v0.36 | 2026-06-15 | @sisibeloved | 新增 `bench_mp_pool` 初始分析和 4C 远端验证：该用例对应 pyperformance `concurrent_imap`，主体是 `multiprocessing.pool.Pool(2).imap(f, range(1000), chunk=10)` 且 `f(x)=x`，收益不来自 JIT 计算本体，而来自避免 Pool bootstrap / job submission 窗口制造低阈值 AutoJIT 固定成本。实现默认 setup provider 扩展为 `lib2to3_main,multiprocessing_pool`，覆盖 `Pool` 构造、context manager 与 `map/imap/imap_unordered/starmap/*_async` 提交方法；明确不包装 `IMapIterator.next` / `ApplyResult.get`，避免把 timed result 消费路径变成逐次 wrapper 成本。blue-98 4C16G cpuset 0-3 复测：旧 `PYTHONJITAUTO=2` `bench_mp_pool=950.8ms`，AutoJIT provider off `133.7ms`，错误 iterator wrap `141.8ms`，最终 Pool.imap-only provider `126.9ms`；`plugin-no-JIT=117.8ms`，no-plugin baseline `120.0ms`。新增 provider 测试并保留 `CINDERX_AUTOJIT_SETUP_PROVIDER=off` 回退。 |
| v0.35 | 2026-06-12 | @sisibeloved | 合并 upstream 后重打 dask 证据：同步本地 tracked 源码到 `blue-98:cinderx-test` 并重装 editable CinderX 后，L1/L3 子集通过；正式同核 `--affinity=30` 复跑 default/noattr，结果 default `1.58s +- 0.06s`、noattr `1.58s +- 0.05s`，`pyperf compare_to` 不显著，说明 !103 descriptor inline cache 后 `PYTHONJITATTRCACHES=0` 小收益消失。per-site deopt 以新 `{pid}` 日志模板重跑：RoiBackoff off `61912`、on `218`，确认 RoiBackoff 止血仍成立但 noattr 方向不再作为当前证据。 |
| v0.34 | 2026-06-11 | @sisibeloved | 补 RoiBackoff 默认开启守门批次：`/results/autojit-roi-backoff-guard-20260611_221949` 对 `2to3`、`deepcopy` 子集、`generators`、`pickle_pure_python`、`sqlalchemy_declarative`、`nbody`、`richards` 做 on/off A/B；`2to3/deepcopy/generators/pickle/sqlalchemy` 无误伤，`nbody/richards` 初始并行差异经同核串行复跑转为 on 更快或持平。结论：支持 RoiBackoff 默认开启，保留 `CINDERX_AUTOJIT_ROI_BACKOFF=0` 回退；gdb smoke 在 blue-98 容器受 seccomp/ptrace 限制，作为环境补验项。 |
| v0.33 | 2026-06-11 | @sisibeloved | 复核 `dask` 后续优化方向：按串行同核 `--affinity=30` 正式复跑默认 AutoJIT 与 `PYTHONJITATTRCACHES=0`，结果 `1.68s -> 1.61s`，`1.04x faster`，确认 noattr 是小正信号但属于全局 JIT 行为变化，不能直接默认化。补 per-site deopt 字段复核：RoiBackoff on 后当前 dask deopt 从 `64164` 降到 `129`，历史 LOAD_ATTR_SLOT 风暴已不是当前主项；关闭 LOAD_ATTR fallback、关闭 array double fastpath 均无收益。策略结论更新为：不继续扩大 startup/provider，也不做全局 slot fallback；下一步只考虑局部 attr-cache/PIC/expected-exception 等 JIT 动态成本专项。 |
| v0.32 | 2026-06-11 | @sisibeloved | 重写 `logging_silent` 复核证据：正式五口径和 HIR/LIR 证明 `Logger.isEnabledFor` 与外层 `bench_silent` call-only loop 进入 CinderX JIT 均为负收益。删除 cached-predicate 静态放行，新增 `CallDispatcher + dims=Object|Dispatch + loop=1 + codeB=1 + risk=None` 的 LowRoi 延迟。正式 `logging_silent` 从原 AutoJIT `328ns` 降到 `212ns`，相对 CPython JIT `219ns` 为 `1.03x faster`；新增 `test_plugin_defers_logging_disabled_fast_path` 与 `test_plugin_defers_call_only_dispatch_loop`。同步补 dask RoiBackoff 复核：`1.67s -> 1.62s`，小收益但非根治。 |
| v0.31 | 2026-06-11 | @guo | 补 `logging_silent` cached predicate 误伤修复证据：`logging.Logger.isEnabledFor` 是 steady-state 缓存命中立即返回、异常慢路径填充 cache 的小 predicate；原 `RiskDefer` 把它冻结在解释器中会放大 disabled logging 百纳秒级快路径成本。新增通用 cached-predicate-with-exception-slow-path 形状放行，不按 logging 文件名/函数名白名单。验证：gate probe 显示 `classified_defer_freeze=0`、`forced_compile>=1`、`Logger.isEnabledFor` 已 JIT；本地复测达到 `logging_silent` 112ns 目标，且修复分支相对同事实验分支只新增该形状规则和测试。 |
| v0.30 | 2026-06-10 | @sisibeloved | 补 `dask` 直接失败修复证据：plain generator steady-state override 误把 `@types.coroutine` 生成的 generator-based coroutine 当普通 generator 放行，导致 `asyncio.tasks:__sleep0` 在 `calls=2 limit=2 reason=None` 下进入 JIT，随后 `asyncio.sleep(0)` 抛 `TypeError: 'generator' object can't be awaited`。修复后排除 `CO_ITERABLE_COROUTINE`/coroutine/async-generator flags；`dask --fast` 通过，compile-events 中不再出现 `__sleep0`，正式 `dask=1.77s +- 0.05s`，相对旧 AutoJIT `1.15x faster`，但仍比 CPython JIT/plugin-no-JIT `1.30x slower`。BehaviorClassifier 42/42、`test_autojit_gate_stats` 8/8 通过。 |
| v0.29 | 2026-06-10 | @sisibeloved | 补 `richards` 误伤修复证据：steady-state 低风险状态 predicate/mutator 与窄 protocol dispatch core 不能按 LowRoi 冻结；只放行带状态写入或 raise 分支的 protocol core，避免把 `pickle_pure_python` setup/call-only wrapper 一起放开。最终子集：`richards=109ms`、`pickle_pure_python=800us`、`2to3=577ms`、`nbody=118ms`；相对 state-helper-only，`richards` `1.14x faster`，`pickle/2to3` 不劣化。BehaviorClassifier 42/42、`test_autojit_gate_stats` 8/8 通过。 |
| v0.28 | 2026-06-10 | @sisibeloved | 补 `generators` 误伤修复证据：`Tree.__iter__` 的 `risk=Exception` 来自 `yield from` generator cleanup，不应按业务异常/动态风险冻结；steady-state 普通 generator 在仅含 `Suspend/Exception` 风险时恢复全局阈值。正式 A/B：破损 AutoJIT `generators=60.2ms`，修复后 `21.7ms`，相对破损 `2.78x faster`，与 `PYTHONJITAUTO=2` 不显著；同口径 `2to3` 改动前 `578ms`、改动后 `579ms`，`pyperf compare_to` 不显著。同步修正 RuntimeTests：BehaviorClassifier 36/36 通过，`test_autojit_gate_stats` 8/8 通过。 |
| v0.27 | 2026-06-10 | @sisibeloved | 补 LowRoi 冻结正式化结果：去掉 spike 环境变量，将 Trivial LowRoi 与长 LowRoi 解释冻结纳入生产策略；正式子集 `pickle_pure_python=800us`、`2to3=991ms`、`nbody=119ms`，相对 v0.26 分别为 `1.16x faster`、`1.16x faster`、`1.07x slower`（nbody/pickle 本轮样本提示不稳），几何均值 `1.08x faster`。 |
| v0.26 | 2026-06-10 | @sisibeloved | 补 `pickle_pure_python` 正式化后数据：AutoJIT 改为不安装 CinderX frame evaluator，并在 `jitVectorcall` 解释返回路径计数；正式子集 `pickle_pure_python=925us`，较优化前 `1056.7us` 拿回约 `131.7us`，但仍未达到 CPython JIT 85% 线。 |
| v0.25 | 2026-06-09 | @sisibeloved | 重写 `pickle_pure_python` 分析：补 CPython JIT / CinderX no-plugin / plugin-no-JIT / `PYTHONJITAUTO=2` / AutoJIT 五口径账本；修正早期未加载 `_cinderx_auto` hook 的无效 CinderX 口径；确认当前劣化主因是 CinderX plugin/frame evaluator 逐帧税，而不是 AutoJIT 分类、import 风暴或 JIT deopt。 |
| v0.24 | 2026-06-09 | @sisibeloved | 重写 `sqlalchemy_declarative` 分析：按 CPython JIT 基线拆出 CinderX 解释器基底、插件固定成本、CinderX JIT 动态成本和 AutoJIT 延迟编译长尾；补 worker-only gate/compile/deopt 证据，确认当前回归不是 import/setup 风暴，而是 `LowRoi=1000` 编译债溢出测量窗口 + ORM steady deopt/guard 动态负 ROI。 |
| v0.23 | 2026-06-09 | @sisibeloved | 新增 `dask` 章节：按 CPython JIT 基线拆成解释器基底、插件固定成本、JIT 动态成本三个增量项，补 gate stats、编译形状和 deopt 动态成本；确认 dask 不是 startup/import compile storm，而是 steady 异步调度 + 序列化路径的 JIT 动态 ROI 负样本。 |
| v0.22 | 2026-06-09 | @sisibeloved | 重写 `2to3` 章节为总/分结构：总表平铺 CPython JIT、优化前、当前候选和 force-interpret 差距；分表拆 gate 函数形状与非 gate 阶段成本，删除旧流水账。 |
| v0.21 | 2026-06-09 | @sisibeloved | 重写 `python_startup` 章节为分阶段平铺账本：四口径（CPython 基线 / `PYTHONJITAUTO=2` 优化前 / 当前 AutoJIT / force interpret-only）逐阶段列 优化前差距/已优化掉/剩余差距/可挖空间，一眼看出差距在哪、优化了多少、还剩多少；后接**全量 17 个 gate 函数形状与策略表**（8 StartupInit + 6 RiskDefer + 2 LowRoi + 1 编译）。实测确认 bc347c29 优化掉 93%（212.6→14.5ms），大头是插件加载 init 期 `schedule_existing_functions` 编译风暴；剩余 +14.5ms（插件地板 ~8 + import 运行税 ~6）无干净单点杠杆。A（逐 def tracking）~0.16ms、C（逐帧计数）硬上界 ≤0.46%、native wrapper ~0.18ms、`jit::initialize` 惰性 ~1.6ms 均穿刺否决。 |
| v0.20 | 2026-06-09 | @sisibeloved | 补充 import/setup 细粒度决策穿刺：拆分 `import_phase`/`setup_phase` 诊断字段并保留 split-only 基础设施；提前 import 分类冻结无稳定收益，暂不引入 import/setup 分叉阈值策略。 |
| v0.19 | 2026-06-08 | @sisibeloved | 补充 startup/site 成本拆解：`_cinderx_exec_impl()`、`jit::initialize()`、`importtime`、gate stats 与 import-depth skip 负向穿刺分表记录；确认 P2a 有成本但不能简单跳过计数。 |
| v0.18 | 2026-06-08 | @sisibeloved | 补充 `2to3` refactor 热点穿刺：profile 确认 parse/pattern 函数是真热点，但 env-gated allow 编译显著负收益，suppress 基本持平；refactor 热点不作为下一步 AutoJIT 策略收益点。 |
| v0.17 | 2026-06-08 | @sisibeloved | 更新 `2to3` 当前候选账本：以 lowROI + cold-bit 作为当前口径，补充 673ms 阶段分布、gate stats、P0 穿刺负结论和下一步优化判断。 |
| v0.16 | 2026-06-08 | @sisibeloved | 再次收敛 `2to3` 账本：6.3 改成纯数据表，解释迁出；新增当前差距速查表，用“差距来源/规模/下一步动作”直接回答当前差距在哪、下一步做什么。 |
| v0.15 | 2026-06-08 | @sisibeloved | 重排 `2to3` 证据结构：先给 CPython JIT 基线、`PYTHONJITAUTO=2` 优化前、当前 AutoJIT、force-interpret 潜力的一页总账；再按 startup/import/tool init/refactor 拆阶段成本、已优化成本和剩余缺口；旧 size100 形状表不再作为主线。 |
| v0.14 | 2026-06-08 | @sisibeloved | 深挖 `2to3` 当前剩余差距：按 import/setup/refactor 阶段交叉现有 shape TSV，并用 phase0 scanner 补查 import 前缀；确认大量 low-risk、`code_bucket=0` 的对象/控制小函数仍按 `2/None` 放行；下一步需要区分 import window 与 setup/refactor window，而不是继续只靠 `startup_phase` 布尔值调参。 |
| v0.13 | 2026-06-08 | @sisibeloved | 补充 `2to3` 生产 no-compile fast path 穿刺：`auto:2` 达到分类点后强制 interpret-only 并恢复 interpreted vectorcall，正式均值 635.5ms，已达到 CPython JIT 85% 目标线；说明当时 974ms 主要来自策略放行后的 residual compile 与低 ROI JIT 动态成本。 |
| v0.12 | 2026-06-08 | @sisibeloved | 补充 `2to3` phase timer 与高全局阈值诊断：CinderX no-plugin 不慢，plugin disabled 只小幅增加；`auto:2097152` 无编译正式口径为 693ms，但该口径在 `jitVectorcall` 早退，不能代表生产 defer fast path，下一步需穿刺“达到 auto:2 后强制 defer 并恢复 interpreted vectorcall”。 |
| v0.11 | 2026-06-08 | @sisibeloved | 按 CPython 3.14.3 JIT 对标口径重写 `2to3` 分析：补充 CPython JIT 549ms 基线、CinderX AutoJIT 各阶段耗时、已吃掉的编译风暴成本和后续深挖方向。 |
| v0.10 | 2026-06-08 | @sisibeloved | 补充 `python_startup` 启动瘦身穿刺证据：Python bootstrap 懒加载和 AutoJIT 初始化跳过已有函数扫描收益明确；`find_and_load` native C wrapper 功能可行但正式 startup 仅改善约 0.18ms，改动较大，暂缓合入。 |
| v0.9 | 2026-06-08 | @sisibeloved | 补充 `sqlalchemy` 系列负 ROI 穿刺：CinderX plugin 但关闭 JIT 已超过 CPython JIT 85% 目标；单纯扩大静态形状延迟只能带来约 1.06x 小收益，且当前 hook 未覆盖真实 worker 的主要编译入口，穿刺代码不建议合入。 |
| v0.8 | 2026-06-08 | @sisibeloved | 根据生产实现 A/B 修正 `deepcopy` 策略：只新增 `_deepcopy_tuple` 对应的 looped expected-exception 形状延迟；`_keep_alive` 放宽会让 `deepcopy_reduce` 明显回归，最终保持既有 `RiskDefer`。 |
| v0.7 | 2026-06-08 | @sisibeloved | 补充 `deepcopy/deepcopy_memo/deepcopy_reduce` deopt 与正式 pyperformance 穿刺：`_deepcopy_tuple` 的 expected `KeyError` deopt 是可收窄对象；`_keep_alive` 虽有 deopt 但保留 JIT 可能有净收益，不能按 try/except 或 deopt 数一刀切。 |
| v0.6 | 2026-06-05 | @sisibeloved | 补充 L3 典型子集 A/B：同一 candidate wheel 上比较 `PYTHONJITAUTO=2` 与 `PYTHONJITAUTO=auto:2 + find_and_load + lib2to3_main`，确认 `2to3`/startup 强收益，严重 JIT 误伤已收敛为小幅回归/噪声项。 |
| v0.5 | 2026-06-05 | @sisibeloved | 补充 `2to3` compute-dominant 策略与 `lib2to3_main` setup provider 复跑证据：incidental `Compute` 不能保护 object/refactor 形状；真实 `python -m lib2to3` setup window 将编译事件从 158 降到 122，性能从 1.315s 提升到 0.968s。 |
| v0.4 | 2026-06-05 | @sisibeloved | 补充 `pickle_pure_python`、`deepcopy` 系列真实 worker gate 证据；明确纯 Python 序列化和深拷贝是 steady 对象图遍历 workload，`LowRoi/RiskDefer` 需要区分“热后编译”和“风险延迟”。 |
| v0.3 | 2026-06-05 | @sisibeloved | 补充 `logging` 系列、`sympy` 系列真实 worker gate 证据；明确 `LowRoi` 是热度延迟而非禁编，steady 高成本符号计算/日志输出路径不能被 import-window 规则粗拦。 |
| v0.2 | 2026-06-05 | @sisibeloved | 补充 `sqlglot_v2` 系列、`sqlalchemy` 系列真实 worker gate 证据；明确这两类用例主要支撑 steady-state highcost 不能全局粗拦、import/setup 只能作为局部延迟条件。 |
| v0.1 | 2026-06-05 | @sisibeloved | 首版。收录 `2to3`、`python_startup`、`coverage`、`generators`、`unpack_sequence` 的已取证函数形状和策略判断；明确 `2to3` 当前最终策略完整形状表仍待补。 |

## 3 字段说明

| 字段 | 含义 |
|---|---|
| 函数 | JIT log 中的 `module:qualname` |
| 编译次数 | 同一用例多个 worker / warmup / values 中该函数被编译的次数 |
| 编译耗时 | 当前证据口径下该函数累计编译耗时；debug 口径只用于排序和形状，不作为正式性能数值 |
| 形状 | `family + dims + loop + codeB + risk + suspend + startup + compute` |
| 策略 | `limit/reason`；如 `2/None` 表示沿用全局阈值 2，`1000/LowRoi` 表示低 ROI 延迟到 1000，`2097152/RiskDefer` 表示风险延迟 |
| 判断 | 对当前策略的维护结论：放行、延迟、拦截、待补或继续观察 |

`codeB` 桶含义：`0:<50`，`1:50-99`，`2:100-499`，`3:>=500` 有效指令。`risk` 可含 `Dynamic`、`Exception`、`HugeCode`、`Suspend`。`dims` 中出现 `Compute` 只是说明函数含有一点计算；只有 `NumericLoop`，或 `Mixed` 且 top-2 维度含 `Compute`，才算 compute-dominant。startup/import/setup 策略不能只因为 incidental `Compute` 放行对象、控制、分发主导的高成本函数。

## 4 策略速查

| 策略结论 | 当前判断 |
|---|---|
| import/setup window 内高成本、非 compute-dominant 函数 | 应延迟，目标是削减 startup/import/setup 编译风暴 |
| steady-state 的 benchmark 热点 | 不能只因 `codeB>0` 或 `risk!=0` 全局拦截，必须看动态收益 |
| `NumericLoop` 或 top-2 `Mixed` 含 `Compute` | 默认放行，除非有明确动态成本证据 |
| 只是 active dims 中出现 `Compute` | 不能当成数值收益保护；若 family 仍是 `ObjectManipulator` / `BranchFSM`，应按主导形状判断 |
| suspendable 状态机 | 不能一刀切拦死；可提高阈值，热度足够时仍应编译 |
| steady-state 普通 generator | 若风险只来自 `Suspend/Exception`，应恢复全局阈值；但必须排除 `CO_ITERABLE_COROUTINE`/coroutine/async-generator，`asyncio.tasks:__sleep0` 是反例 |
| steady-state 状态 predicate/mutator | 低风险、小 code、纯状态读/写/布尔判断应恢复全局阈值；这是 `richards` 的核心收益点之一 |
| steady-state protocol dispatch core | 只放行小型、低风险、有对象状态访问、控制流和返回值，并且有状态写入或 raise 分支的核心方法；call-only wrapper 继续 LowRoi |
| 纯 `ObjectManipulator` 大函数 | import window 可延迟；steady-state 中可能是核心热点，不能全局拦 |
| expected exception 作为正常控制流 | 需要结合 deopt 和 A/B 单独判断；不能只因有 try/except、`risk=Exception` 或 deopt 数高就禁编 |
| `RiskDefer` | 只说明风险成本高；上线前必须证明省下的静态成本大于丢掉的动态收益 |

## 5 口径索引

| 用例 | 口径 | 数据来源 | 说明 |
|---|---|---|---|
| L3 典型子集 | A/B：baseline=`PYTHONJITAUTO=2`，candidate=`PYTHONJITAUTO=auto:2` + `CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` + `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main`，`--warmup 3 --affinity=30` | `blue-98:cinderx-test:/results/autojit-l3-subset-compute-provider-20260605/{baseline-auto2.json,candidate-auto-classify-provider.json,compare-table.txt}` | 同一 candidate wheel 内比较分类策略开/关；17 个 selector 展开为 25 个 compare row；几何均值 1.13x faster，主要由 `2to3`/startup 拉动 |
| `2to3` | CPython 3.14.3 JIT 基线，`--warmup 3 --affinity=30`，`PYTHON_JIT=1` | `blue-98:cpython-baseline:/results/cpython-jit-2to3-20260608/{cpython_jit_2to3_aff30.json,cpython_jit_2to3_aff30.log,cpython_jit_2to3_aff30_stats.txt}` | 基线均值 `549ms`；当前 lowROI + cold-bit 候选约 `673ms`，距 85% 目标线约 `+27ms`；历史 setup provider 口径为 `956-973ms` |
| `2to3` | `PYTHONJITAUTO=auto:2`，早期 `size100` 策略形状表 | `blue-98:/results/autojit-compile-lists-20260605/2to3.shape.tsv` | 有完整函数形状，共 122 个编译事件；不是最终 `highcost > 0` 口径 |
| `2to3` | 当前 `highcost > 0` import-window 策略 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/2to3-direct-debug.jit.log` | 有编译数和编译耗时，完整形状表待补 |
| `2to3` | compute-dominant 修正，无 setup provider，`PYTHONJITAUTO=auto:2` | `blue-98:/results/autojit-compute-dominant-20260605/2to3-candidate.json`；debug: `.../current-2to3-debug/2to3.jit.log` | 正式均值 1.315s；debug 158 个编译事件、累计 483.207ms |
| `2to3` | runpy 原型：整个 `lib2to3` main/refactor 窗口复用现有 depth | `blue-98:/results/autojit-compute-dominant-20260605/prototype-main-window-debug/2to3-main-window.jit.log` | debug 118 个编译事件、累计 187.628ms；证明需要 setup/main window 数据源 |
| `2to3` | 真实 `python -m lib2to3`，`CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main` | `blue-98:/results/autojit-compute-dominant-20260605/2to3-setup-provider.json`，final: `.../2to3-setup-provider-final.json`；debug: `.../setup-provider-debug/2to3-setup-provider.jit.log` | 正式均值 0.968s；final wheel 复跑 0.965s 但 pyperf 标记样本稳定性 warning；debug 122 个编译事件、累计 196.548ms |
| `2to3` | 历史 `interpret-only` / no-compile 穿刺 | `blue-98:/results/autojit-l2-conservative-20260604/interpret-only-auto2-aff30.json` | 正式均值约 0.966s；与历史 setup provider 结果接近，只作为“剩余差距可能不全是编译”的提示，后续已被 force interpret-only 穿刺替代 |
| `2to3` | phase timer + 高全局阈值诊断，`--affinity=30`，20 次 direct bench_command；`auto:2097152` 正式 pyperformance 60 values | `blue-98:/root/cinderx-lab/results/autojit-2to3-phase-20260608/{phase_summary_table.md,phase_compile_table.md,cinderx_auto2_phase.jsonl,cinderx_autojit_nocompile_2to3_aff30.json,autojit_phase_compile_summary.json}` | `PYTHONJITAUTO=2` phase timer 为 `4108.0ms`，与正式 `4097ms` 对齐；`cinderx_no_plugin=516.7ms`，`plugin_jitdisabled=527.6ms`，`auto:2097152=689.6ms/正式693ms`，`auto:2=974.0ms`；`auto:2097152` 证明“巨大全局阈值”会让每次调用反复进 `jitVectorcall`，不等价于生产 defer fast path |
| `2to3` | 生产 no-compile fast path 穿刺：`auto:2` 达到分类点后强制 interpret-only 阈值并恢复 interpreted vectorcall | `blue-98:/root/cinderx-lab/results/autojit-2to3-phase-20260608/{cinderx_autojit_force_interpret_2to3_aff30.json,cinderx_autojit_force_interpret_phase_summary.json,autojit_force_interpret_debug.jit.log}` | phase timer `632.8ms`，正式 pyperformance `635.5ms`，debug 编译数 0；低于 CPython JIT 85% 目标线 `646ms`，证明生产 defer fast path 成本可接受 |
| `2to3` | 当前候选：lowROI 收窄 + classified warmup return + cold-bit 计数早退 | `blue-98:/results/autojit-p0-spike-20260608/{cold-bit-provider-2to3.json,cold-bit-phase-summary.json,cleaned-retained-candidate-build.log}` | 正式 pyperformance `673ms +- 1ms`，phase timer `673.3ms`；距 CPython JIT 85% 目标线约 `+27ms`；forced compile 中位数降到 11 |
| `2to3` | import/setup split-only：`GateContext` 拆出 `import_phase`、`setup_phase`，compile event 打细分阶段；策略仍按合并 `startup_phase` 执行 | `blue-98:/results/autojit-p0-spike-20260608/{import-setup-split-clean-2to3.json,import-setup-split-clean-phase.jsonl,import-setup-split-clean-phase-gate-stats.jsonl,import-setup-split-diag-compile-events.jsonl}` | 正式 pyperformance `679ms +- 1ms`，phase timer `675.8ms`；gate 分布与当前候选同量级；1 次诊断事件中 `phase` 可分出 `steady=7/import=7/setup=2`；作为后续分阶段决策基础保留 |
| `2to3` | setup/main window 扩大穿刺 | `blue-98:/results/autojit-p0-spike-20260608/{p0-main-setup-provider-2to3.json,current-candidate-main-setup-phase-summary.json}` | 正式 `765ms +- 11ms`，phase timer `777.7ms`；forced compile 从 11 增到约 77-79，负收益，已回滚 |
| `2to3` | P0 固定成本穿刺：C 侧 import wrapper、lazy `cinderjit` | `blue-98:/results/autojit-p0-spike-20260608/{c-import-wrapper-provider-2to3.json,lazy-cinderjit-provider-2to3.json,c-import-wrapper-phase-summary.json,lazy-cinderjit-phase-summary.json}` | C wrapper 正式 `673ms +- 2ms`，lazy `cinderjit` 正式 `675ms +- 2ms`；相比当前候选无稳定收益，已回滚 |
| `2to3` | refactor 热点 allow/suppress 穿刺 | `blue-98:/results/autojit-p0-spike-20260608/refactor-hotspot-phase/*`；profile: `.../refactor-hotspot-profile/current-candidate.prof` | 当前 off phase `669.3ms`；`allow-parse=876.5ms`、`allow-pattern=885.9ms`、`allow-all=1081.0ms` 明显负收益；`suppress-all=670.5ms` 基本持平，穿刺代码已回滚 |
| `2to3` | `generators` 修复对 `2to3` 的提交前后 A/B：同一 `cinderx-test` 容器、同一 `/opt/python314`、`CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 PYTHONJITHUGEPAGES=0`、`--warmup 3 --affinity=30`，只切换 plain generator steady-state 放行策略 | `blue-98:cinderx-test:/results/autojit-generators-regression-20260610/{2to3-before-generator-policy.json,2to3-after-generator-policy.json}` | 改动前 `578ms +- 1ms`，改动后 `579ms +- 5ms`；`pyperf compare_to` 标记 hidden as not significant。结论：plain generator 放行没有测出 `2to3` 劣化 |
| `2to3` / startup-site | startup/site 微拆解：`-c pass`、早期 stdlib prelude、`importtime`、`_cinderx_exec_impl()`、`jit::initialize()` | `blue-98:/results/autojit-p0-spike-20260608/startup-site-breakdown/*` | 当前候选下 plugin 固定启动税约 `13.6ms`，早期 stdlib import 额外税约 `10.7ms`；`_cinderx_exec_impl()` 约 `3.3ms`，`jit::initialize()` 约 `1.84ms` |
| `2to3` / startup-site | import-depth skip 计数早退穿刺 | `blue-98:/results/autojit-p0-spike-20260608/startup-site-breakdown/{import_skip_*.json,import-skip-2to3-*-summary.json,gate-import-skip-*.jsonl,import-skip-2to3-gate.jsonl}` | prelude 从 `57.5ms` 降到 `51.6ms`，但 `2to3` full 退到 `849.7ms`；`jit_vectorcall` 暴涨到 `607856`，穿刺已回滚 |
| `python_startup` | `PYTHONJITAUTO=auto:2` | `blue-98:/results/autojit-compile-lists-20260605/python_startup.shape.tsv` | 有完整函数形状，共 2 个编译事件 |
| `python_startup` | CPython 3.14.3 JIT 基线，`--warmup 3 --affinity=30` | `blue-98:/root/cinderx-lab/results/autojit-startup-breakdown-20260608/cpython_jit_startup_aff30.json` | 基线：`python_startup=18.128ms`，`python_startup_no_site=11.740ms` |
| `python_startup` | Python bootstrap 懒加载 + AutoJIT 初始化跳过已有函数扫描，`auto:2 + find_and_load` | `blue-98:/root/cinderx-lab/results/autojit-startup-init-spike-20260608/{cinderx_auto_classify_startup_init_spike_aff30_valid.json,importtime/*,process_time_init_spike.tsv}` | 正式均值：`python_startup=38.617ms`，`python_startup_no_site=13.358ms`；no-site 已达 CPython JIT 87.9%，site 仍只有 46.9% |
| `python_startup` | `find_and_load` provider 改为 `_cinderx` native C wrapper 穿刺 | `blue-98:/root/cinderx-lab/results/autojit-native-import-provider-20260608/{cinderx_auto_native_provider_startup_aff30.json,cinderx_auto_native_provider_2to3_aff30.json,importtime/*}` | 功能可行，但正式 `python_startup=38.440ms`，只比上一轮快约 0.18ms；`2to3=956ms` 与既有 966/973ms 同量级，暂不作为优先合入项 |
| `python_startup` | §7 分阶段账本 + 全量 gate 形状（四口径 wall 中位数 + importtime 窗口 + 17 函数 shape，debug 口径） | `blue-98:cinderx-test:/results/autojit-startup-ledger-20260609/{startup_totals.tsv,importtime_windows.tsv,python_startup.gate-shapes.raw.log,python_startup.gate-shapes.tsv,README.md}` | 同一 `_cinderx.so` 只改环境：base/jit2/auto/nocompile = 21.8/234.4/35.9/31.7ms（site）；`_cinderx_auto` importtime 累计 0.4/185.5/8.5/8.1ms；全量 17 个 gate 函数 = 8 StartupInit + 6 RiskDefer + 2 LowRoi + 1 编译。README 含复现命令与枚举映射 |
| `dask` | 正式 pyperformance 五口径：CPython 3.14.3 JIT、CinderX no-plugin、CinderX plugin-no-JIT、`PYTHONJITAUTO=2`、当前 AutoJIT；`--warmup 3 --affinity=30` | `blue-98:cinderx-test:/results/autojit-dask-ledger-20260609/{cpython_jit_dask_aff30.json,cinderx_no_plugin_dask_aff30.json,cinderx_plugin_nojit_dask_aff30.json,cinderx_auto2_dask_aff30.json,cinderx_autojit_dask_aff30.json,logs/*}` | CPython JIT `1.360s`；CinderX no-plugin `1.289s`；plugin-no-JIT `1.355s`；`auto=2` `2.005s`；当前 AutoJIT `2.022s`。差距主要来自启用 CinderX JIT 后新增约 `+650ms`，不是插件固定成本 |
| `dask` | debug shape/gate/deopt：`--fast -n 3 -w 1`，`PYTHONJITLOGFILE` + `CINDERX_AUTOJIT_GATE_STATS_FILE` + `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE` | `blue-98:cinderx-test:/results/autojit-dask-ledger-20260609/{dask-gate-stats.jsonl,dask-compile-events.jsonl,worker-gate-shapes/*.jit.log,dask-debug-fast.json}` | 4 个 worker 合计约 `962700` 次 gate、`3408` 次 forced compile、`161135` 次 defer freeze；编译事件 `3485` 条中 `3440` 条在 steady 阶段；deopt 合计 `1020754` 次，主要是 `GuardFailure` 和 expected exception |
| `dask` | plain generator override 误伤复现与修复：`PYTHONJITAUTO=auto:2`，默认 provider，`PYTHONJITHUGEPAGES=0`；失败/修复均附 compile-events | `blue-98:cinderx-test:/results/autojit-dask-failure-20260610/{dask-fast-autojit.log,dask-fail-compile-events.jsonl,dask-fast-autojit-fixed.json,dask-fixed-compile-events.jsonl,dask-formal-autojit-fixed.json,logs/*}` | 失败口径中 `asyncio.tasks:__sleep0` 以 `calls=2 limit=2 reason=None` forced compile，触发 `TypeError: 'generator' object can't be awaited`；修复后 `dask --fast` 通过且 fixed compile-events 不再含 `__sleep0`；正式 `dask=1.77s +- 0.05s`，相对旧 AutoJIT `1.15x faster`，仍比 CPython JIT/plugin-no-JIT `1.30x slower` |
| `dask` | RoiBackoff 后 per-site deopt 与 noattr 正式复核：默认 AutoJIT vs `PYTHONJITATTRCACHES=0`，串行同核 `--affinity=30 --warmup 3`，无 dump/HIR/debug 变量 | `blue-98:cinderx-test:/results/autojit-dask-site-fields-{roion,roioff}-20260611_203926`；`blue-98:cinderx-test:/results/autojit-dask-noattr-serial-aff30-20260611_212203/{default.json,noattr.json,compare-noattr-vs-default.txt,*.stats.txt,logs/*}` | 当前 RoiBackoff on 后 deopt 只剩 `129`，off 为 `64164`；历史 LOAD_ATTR_SLOT 风暴已不是当前主项。正式同核复跑默认 `1.68s +- 0.05s`，noattr `1.61s +- 0.04s`，`1.04x faster`；noattr 是小正信号但为全局 JIT 开关，不能直接作为生产默认 |
| `dask` | 合并 upstream 后正式复核：default AutoJIT vs `PYTHONJITATTRCACHES=0`，串行同核 `--affinity=30 --warmup 3`；per-site deopt 使用 `{pid}` 日志模板、`--fast --warmup 1` | `blue-98:cinderx-test:/results/autojit-dask-after-upstream-20260612_003254/{default.json,noattr.json,compare-noattr-vs-default.txt}`；`blue-98:cinderx-test:/results/autojit-dask-site-fields-after-upstream-20260612_004151-pid/{roioff,roion}` | default `1.58s +- 0.06s`，noattr `1.58s +- 0.05s`，不显著；RoiBackoff off/on deopt `61912 -> 218`。结论：!103 后 noattr 证据失效，RoiBackoff 止血仍成立，剩余差距需重新拆账 |
| RoiBackoff 默认开启守门 | off=`CINDERX_AUTOJIT_ROI_BACKOFF=0`，on=`CINDERX_AUTOJIT_ROI_BACKOFF=1`，其余 `CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2`，`--warmup 3`；`nbody/richards` 追加同核串行复跑 | `blue-98:cinderx-test:/results/autojit-roi-backoff-guard-20260611_221949/{off,on}` | `2to3/deepcopy/generators/pickle_pure_python/sqlalchemy_declarative` 无误伤；`nbody/richards` 初始并行差异由 CPU/NUMA 噪声解释，同核串行 on 分别为 `0.94x/0.99x` off；支持默认开启，保留 `=0` 回退 |
| `coverage` | direct worker，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/coverage.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `generators` | direct worker，同上 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/generators.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `generators` | 误伤复现与修复 A/B：`PYTHONJITAUTO=auto:2`，默认 provider；对照 `PYTHONJITAUTO=2`、破损 AutoJIT、修复后 AutoJIT；附 compile-events 验证 `Tree.__iter__` 是否进入 JIT | `blue-98:cinderx-test:/results/autojit-generators-regression-20260610/{generators-cinderx-auto2.json,generators-cinderx-autojit.json,generators-cinderx-autojit-fixed.json,generators-autojit-compile-events.jsonl,generators-autojit-fixed-compile-events.jsonl}` | `PYTHONJITAUTO=2` `21.7ms +- 6.2ms`；破损 AutoJIT `60.2ms +- 0.3ms`；修复后 `21.7ms +- 5.8ms`。修复后相对破损 `2.78x faster`，与 `auto=2` 不显著；compile-events 确认修复后 `Tree.__iter__ limit=2 reason=None` |
| `richards` / `pickle_pure_python` / `2to3` / `nbody` | protocol dispatch core 收窄后的正式子集：`PYTHONJITAUTO=auto:2`，默认 import/setup provider，`PYTHONJITHUGEPAGES=0`，`--warmup 3 --affinity=30` | `blue-98:cinderx-test:/results/autojit-protocol-core-20260610/protocol-core-store-or-raise-richards-pickle-2to3-nbody.json` | 最终结果：`richards=109ms +- 7ms`、`pickle_pure_python=800us +- 3us`、`2to3=577ms +- 1ms`、`nbody=118ms +- 14ms`。相对 state-helper-only：`richards` `1.14x faster`，`pickle/2to3` 不显著劣化；`nbody` 样本不稳 |
| `richards` | protocol dispatch core 编译事件复核 | `blue-98:cinderx-test:/results/autojit-protocol-core-20260610/{richards-store-or-raise-compile-events.jsonl,richards-store-or-raise-events-run.json}` | 最终策略新增编译 7 个 benchmark 主体函数：`DeviceTask.fn`、`HandlerTask.fn`、`IdleTask.fn`、`Task.addPacket`、`Task.findtcb`、`TaskState.isTaskHoldingOrWaiting`、`TaskState.isWaitingWithPacket`；单用例 event run `104ms +- 1ms` |
| `unpack_sequence` | direct worker，同上 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/unpack_sequence.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlalchemy_declarative` | direct worker，`--fast --values=3 --warmups=1`，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlalchemy_declarative.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `sqlalchemy_imperative` | direct worker，同上 | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlalchemy_imperative.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlalchemy` 系列 | CPython 3.14.3 JIT 基线 | `blue-98:/results/cpython-jit-sqlalchemy-20260608/cpython_jit.json` | 目标口径基线：`sqlalchemy_declarative=271ms`，`sqlalchemy_imperative=47.1ms`；85% 目标分别为 319ms、55.4ms |
| `sqlalchemy` 系列 | CinderX plugin，JIT disabled | `blue-98:/results/autojit-sqlalchemy-analysis-20260608/cinderx_plugin_jitdisabled.json` | 关闭 CinderX JIT 后 `declarative=245ms`、`imperative=40.9ms`，说明本组用例对 CinderX JIT 是负 ROI，解释器/plugin 路径本身已超过 CPython JIT 目标 |
| `sqlalchemy` 系列 | CinderX `PYTHONJITAUTO=2` | `blue-98:/results/autojit-sqlalchemy-analysis-20260608/auto2.json` | `declarative=388ms`、`imperative=73.7ms`，相对 CPython JIT 几何均值约 1.50x slower |
| `sqlalchemy` 系列 | high-cost object/control 静态延迟穿刺 | `blue-98:/results/autojit-sqlalchemy-object-branch-spike-20260608/{candidate_sqlalchemy_spike.json,candidate_sqlalchemy_v4.json,shape-logs-v4/*}` | v1 只延迟 `BranchFSM`，v4 扩到 `BranchFSM/ObjectManipulator/ReflectionMeta/Mixed`；正式结果仅相对 `auto=2` 约 1.06x faster，仍相对 CPython JIT 约 1.42x slower |
| `sqlalchemy_declarative` | 正式 pyperformance 五口径：CPython 3.14.3 JIT、CinderX no-plugin、CinderX plugin-no-JIT、`PYTHONJITAUTO=2`、当前 AutoJIT；`--warmup 3 --affinity=30` | `blue-98:cinderx-test:/results/autojit-sqlalchemy-declarative-ledger-20260609/{cpython_jit_sqlalchemy_declarative_aff30.json,cinderx_no_plugin_sqlalchemy_declarative_aff30.json,cinderx_plugin_nojit_sqlalchemy_declarative_aff30.json,cinderx_auto2_sqlalchemy_declarative_aff30.json,cinderx_autojit_sqlalchemy_declarative_aff30.json}` | CPython JIT `269.2ms`；CinderX no-plugin `245.5ms`；plugin-no-JIT `249.8ms`；`auto=2` `387.8ms`；当前 AutoJIT mean `434.4ms` / median `377.5ms`。回归来自 CinderX JIT 动态成本和 AutoJIT 延迟编译长尾，不是插件固定成本 |
| `sqlalchemy_declarative` | debug shape/gate/deopt：`--fast --warmup 1`，`PYTHONJITLOGFILE` + `CINDERX_AUTOJIT_GATE_STATS_FILE` + `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE` | `blue-98:cinderx-test:/results/autojit-sqlalchemy-declarative-ledger-20260609/{sqlalchemy_declarative-gate-stats.jsonl,sqlalchemy_declarative-compile-events.jsonl,worker-gate-shapes/sqlalchemy_declarative.%p.jit.log,sqlalchemy_declarative-debug-fast.json}` | worker-only 合计约 `403291` 次 gate、`2127` 次 forced compile、`358240` 次 classified warmup；编译事件 `2134` 条中 `2099` 条在 steady；deopt 合计 `22133` 次，`GuardFailure` 占 `21714` |
| `sqlglot_v2` | direct worker，同上，`bm_sqlglot_v2 normalize` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlglot_v2.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlglot_v2_parse` | direct worker，同上，`bm_sqlglot_v2 parse` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlglot_v2_parse.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlglot_v2_transpile` | direct worker，同上，`bm_sqlglot_v2 transpile` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlglot_v2_transpile.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlglot_v2_optimize` | direct worker，同上，`bm_sqlglot_v2 optimize` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlglot_v2_optimize.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `logging_silent` | direct worker，`--fast --values=3 --warmups=1`，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load`，`bm_logging silent` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/logging_silent.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `logging_simple` | direct worker，同上，`bm_logging simple` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/logging_simple.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `logging_format` | direct worker，同上，`bm_logging format` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/logging_format.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sympy_expand` | direct worker，同上，`bm_sympy expand` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/sympy_expand.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sympy_integrate` | direct worker，同上，`bm_sympy integrate` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/sympy_integrate.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sympy_sum` | direct worker，同上，`bm_sympy sum` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/sympy_sum.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sympy_str` | direct worker，同上，`bm_sympy str` | `blue-98:/results/autojit-logging-sympy-shapes-20260605/worker-gate-shapes/sympy_str.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `pickle_pure_python` | direct worker，`--fast --values=3 --warmups=1`，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load`，`bm_pickle --pure-python pickle` | `blue-98:/results/autojit-pickle-deepcopy-shapes-20260605/worker-gate-shapes/pickle_pure_python.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `pickle_pure_python` | 正式 pyperformance 五口径：CPython 3.14.3 JIT、CinderX no-plugin、CinderX plugin-no-JIT、`PYTHONJITAUTO=2`、当前 AutoJIT；`--warmup 3 --affinity=30` | `blue-98:{cpython-baseline,cinderx-test}:/results/autojit-pickle-ledger-20260609/{cpython_jit_pickle_pure_python_aff30.json,cinderx_no_plugin_pickle_pure_python_real_aff30.json,cinderx_plugin_nojit_pickle_pure_python_autohook_aff30.json,cinderx_auto2_pickle_pure_python_autohook_aff30.json,cinderx_autojit_pickle_pure_python_autohook_aff30.json}` | CPython JIT `684.0us`；CinderX no-plugin `643.8us`；plugin-no-JIT `1058.9us`；`auto=2` `1058.4us`；当前 AutoJIT `1056.7us`。差距来自启用 CinderX plugin 后的逐帧税；JIT/AutoJIT 基本没有增量影响 |
| `pickle_pure_python` / `2to3` / `nbody` | 正式化后 pyperformance 子集：`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load`，`--warmup 3 --affinity=30`，无 `CINDERX_SPIKE_*` | `blue-98:cinderx-test:/results/autojit-formal-vectorcall-count-20260610/{candidate_auto2_find_pickle_2to3_nbody.json,logs/candidate_subset_pickle_2to3_nbody_v2.log}` | `pickle_pure_python=925us`，`2to3=1.15s`，`nbody=111ms`；worker venv 已确认 `include-system-site-packages=true`，系统 `cinderx.pth` 生效，`_cinderx_auto` 自动加载 |
| `pickle_pure_python` / `2to3` / `nbody` | LowRoi 冻结正式化后 pyperformance 子集：`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load`，`--warmup 3 --affinity=30`，无 `CINDERX_SPIKE_*` | `blue-98:cinderx-test:/results/autojit-lowroi-freeze-formal-20260610/candidate_auto2_find_pickle_2to3_nbody_cinderx.json` | `pickle_pure_python=800us`，`2to3=991ms`，`nbody=119ms`；同轮先发现无效结果来自新 venv `include-system-site-packages=false`，修正并 smoke 确认 `_cinderx_auto=True`、`cinderjit=True`、provider=`find_and_load` 后重跑 |
| `pickle_pure_python` | debug shape/gate/deopt：临时 autohook `sitecustomize.py` 只 `import _cinderx_auto`，`bm_pickle --pure-python pickle --fast -n 3 -w 1`，`PYTHONJITLOGFILE` + `CINDERX_AUTOJIT_GATE_STATS_FILE` + `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE` | `blue-98:cinderx-test:/results/autojit-pickle-ledger-20260609/{pickle_pure_python-gate-stats.jsonl,pickle_pure_python-compile-events.jsonl,worker-gate-shapes/pickle_pure_python.%p.jit.log,pickle_pure_python-debug-fast-autohook.json}` | 5 个 worker/driver 记录合计 `49024` 次 gate、`39002` 次 classified warmup、`372` 次 forced compile；编译事件 `377` 条中 `352` 条在 steady；deopt 只有 pyperf harness 的 3 次 `Raise`，没有 pickle 主体 deopt |
| `deepcopy` | direct worker，同上，`bm_deepcopy` 一次 run 覆盖 `deepcopy/deepcopy_reduce/deepcopy_memo` | `blue-98:/results/autojit-pickle-deepcopy-shapes-20260605/worker-gate-shapes/deepcopy.*.jit.log` | 有真实 C++ gate 形状；debug 口径；函数表按 benchmark-self 归因 |
| `deepcopy` | deopt probe + 正式 pyperformance，`PYTHONJITAUTO=2`、`PYTHONJITAUTO=auto:2 + find_and_load`、只抑制 `_deepcopy_tuple` 三组对照 | `blue-98:cinderx-test:/results/autojit-deepcopy-deopt-20260608/{deopt-auto2.log,deopt-auto-classify.log,current.log,suppress_tuple.log,suppress_tuple_keep.log,formal-*.json,compare-*.txt}` | 验证 `_deepcopy_tuple` 与 `_keep_alive` 都会因 `DictSubscr` expected `KeyError` 产生 `UnhandledException` deopt；但策略收益不同，不能用一个异常风险规则同时处理 |

### 5.1 L3 典型子集 A/B 结论

本轮 A/B 不使用 CPython JIT 基线；远端当前 `/opt/python314/bin/python3` 的 `sys._jit.is_available()` 为 `False`。本轮只回答“本功能相对原始 CinderX 低阈值 `PYTHONJITAUTO=2` 是否有收益/回归”。baseline 与 candidate 使用同一 candidate wheel、同一 worker venv、同一 `--warmup 3 --affinity=30`，唯一差异是 `PYTHONJITAUTO=2` vs `PYTHONJITAUTO=auto:2 + provider`。

| 结论 | 用例 | A/B 结果 | 判断 |
|---|---|---:|---|
| 强收益 | `2to3` | 4.10s -> 966ms，4.24x faster | setup provider 覆盖 main/refactor 初始化窗口后，编译风暴收益明确 |
| 强收益 | `python_startup_no_site` | 90.8ms -> 15.1ms，6.03x faster | startup/import 延迟策略命中主目标 |
| 强收益 | `python_startup` | 225ms -> 55.8ms，4.03x faster | startup/import 延迟策略命中主目标 |
| 严重误伤已收敛 | `unpack_sequence` | 8.70ns -> 7.63ns，1.14x faster | 之前的大幅劣化已消失 |
| 严重误伤已收敛 | `fannkuch` | hidden as not significant | 之前的大幅劣化已消失或降到噪声 |
| 严重误伤已收敛 | `scimark_fft` | 316ms -> 300ms，1.05x faster | 数值/compute-dominant 保护有效 |
| 轻微回归/待查 | `coverage` | 25.4ms -> 27.9ms，1.10x slower | 小幅回归，需看是否分类延迟了 benchmark 本体外的 steady helper |
| 轻微回归/待查 | `generators` | 17.5ms -> 20.0ms，1.14x slower | candidate 方差较大，需复跑或看 gate log |
| 轻微回归/待查 | `dulwich_log` | 171ms -> 180ms，1.05x slower | 业务型样本轻微回归，优先看 steady object/dispatch helper 是否被延迟 |
| 轻微回归/待查 | `sqlglot_v2_normalize` | 382ms -> 432ms，1.13x slower | steady parser/optimizer 高成本函数可能被策略过度延迟 |
| 长尾回归/待优化 | `sqlalchemy_declarative` | `auto=2` 387.8ms -> AutoJIT 434.4ms mean / 377.5ms median | 复跑确认是双峰长尾：低位值快于 `auto=2`，但 `LowRoi=1000` 延迟编译债溢出 measured values；不是 import/setup 风暴 |
| 已定位并修复 | `logging_silent` | 原 AutoJIT 328ns -> 最终 212ns，1.55x faster；相对 CPython JIT 219ns 为 1.03x faster | 主因是 `Logger.isEnabledFor` 和外层 call-only loop 被 JIT 后负收益；最终策略改为二者解释执行 |
| 噪声/基本持平 | `dask`、`deepcopy_reduce` | hidden as not significant | 暂不作为策略调整依据 |

总体几何均值为 1.13x faster，但主要由 `2to3` 和 startup 拉动。下一步策略优化不应再围绕 startup/import 风暴泛化，而应针对 steady-state 轻微回归样本复跑 gate log，确认是否存在过度 `LowRoi/RiskDefer`。

## 6 用例：2to3

### 6.1 总表：差距账本

`2to3` 的对标目标是：开启 CinderX JIT 后，性能恢复到 CPython 3.14.3 JIT 的 85% 以上。CPython JIT phase 基线是 `549.7ms`，85% 目标线约 `646.7ms`。本节 phase 账本对应 v0.17 cold-bit 候选：`673.3ms`，当时还差约 `26.6ms`。最新 v0.28 pyperformance 口径已降到约 `578-579ms`，但本轮没有重打 phase timer，因此这里只保留旧 phase 分解，并在 §5 记录最新提交前后 A/B。

下表统一使用 phase timer 中位数，单位为 `ms`。`force interpret-only` 是“达到分类点后不编译并恢复解释执行”的穿刺下限，用来估算当前候选还剩多少 AutoJIT/gate 成本可挖，不代表承诺收益。

| 阶段 | CPython JIT 基线 | CinderX JIT 优化前<br/>`PYTHONJITAUTO=2` | 优化前差距 | 当前候选<br/>lowROI + cold-bit | 已优化掉 | 当前剩余差距 | 相对 force interpret-only<br/>仍可挖 | 下一步判断 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| startup/site | 75.2 | 1385.7 | +1310.5 | 108.1 | 1277.6 | +32.9 | 19.5 | 继续拆固定启动税、frame evaluator/计数税 |
| import `lib2to3.main` | 53.9 | 747.9 | +694.0 | 64.9 | 683.0 | +11.0 | 3.8 | 大头已解决，不做提前 import 分类冻结 |
| tool init / fixer setup | 128.9 | 834.5 | +705.6 | 161.9 | 672.5 | +33.0 | 8.6 | 还有小账；粗扩 setup window 已证明负收益 |
| refactor 输入文件 | 289.0 | 1032.5 | +743.5 | 332.3 | 700.2 | +43.3 | 5.4 | 剩余差距最大，但白名单 parse/pattern JIT 已证明负收益 |
| 其它 glue | 2.7 | 107.5 | +104.8 | 6.1 | 101.3 | +3.4 | 3.3 | 小项，低优先级 |
| 合计 | 549.7 | 4108.0 | +3558.3 | 673.3 | 3434.7 | +123.6 | 40.5 | 已解决原始差距约 96.5%，还差 85% 线约 26.6ms |

| 关键问题 | 当前答案 |
|---|---|
| CinderX JIT 优化前为什么慢 | 不是单一阶段慢，而是 startup/import/tool/refactor 都被低阈值编译风暴放大。 |
| 当前为什么还没到 85% 线 | 大编译风暴已经消失，剩下是 `+123.6ms` 小账；其中能被 force-interpret 下限解释的只有约 `40.5ms`。 |
| 下一步最该看哪里 | 若只追 85% 线，优先看 startup/site 的 `19.5ms` 可挖空间，其次是 tool init 的 `8.6ms`。 |
| 当前不该继续做什么 | 不该粗扩 setup window，不该白名单放行 refactor parse/pattern 热点，不该简单跳过 import 期间计数。 |

### 6.2 分表一：进入 gate 的函数形状

数据来源：`blue-98:/results/autojit-p0-spike-20260608/cold-bit-compile-events.jsonl` 与同轮 gate stats。当前候选单次 `2to3` phase run 的典型 gate 路径如下。

| gate 路径 | 次数 | 含义 |
|---|---:|---|
| `jit_vectorcall` | 2919 | 进入 AutoJIT gate 的总次数 |
| `global_threshold_return` | 1119 | 还没到全局阈值，只计数后返回解释执行 |
| `classified_warmup_return` | 10 | 已分类，但阈值判断要求继续解释等待 |
| `classified_defer_freeze` | 1780 | 分类后判定延迟/解释执行，并恢复 interpreted vectorcall |
| `forced_compile` / `forced_compile_ok` | 10 / 10 | 真正进入 CinderX JIT 编译 |
| `fallback` | 0 | 没有 gate 异常回退 |

分类本身的开销主要来自 structure key cache miss 扫描。

| 分类路径 | 次数 | 总耗时 | 单次约 | 判断 |
|---|---:|---:|---:|---|
| `classify_block` | 1771 | 16.9ms | 9.5us | 分类总成本有账，但不是几百毫秒主因 |
| `structure_key_lookup` | 1700 | 16.1ms | 9.4us | lookup 成本基本等于 miss 扫描成本 |
| `structure_key_cache_hit` | 1294 | 0.08ms | 0.07us | 命中成本可忽略 |
| `structure_key_cache_miss` | 406 | 16.0ms | 39.4us | 当前唯一值得继续看的 gate 内小账 |
| `compute_threshold` | 1700 | 0.15ms | 0.09us | 阈值计算不是瓶颈 |

scanner 微优化穿刺已经验证：`opcode` 查表化 + 只在 Control 类 opcode 上调用 `isBranch()`，把 miss 扫描从约 `15.98ms` 降到 `15.05ms`，只省约 `0.9ms`，暂不作为主线提交。

当前候选真正被编译的函数如下。多数是 `codeB=0` 的小函数；当前编译列表里没有 refactor parse/pattern 大热点。

| 阶段 | 函数 | 事件数 | family | dims | loop | codeB | risk | limit | reason |
|---|---|---:|---|---|---:|---:|---|---:|---|
| `steady` | `_frozen_importlib_external:FileLoader.__init__` | 21 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `_sitebuiltins:_Printer.__init__` | 21 | `ObjectManipulator` | `Control+Object` | 2 | 0 | `-` | 2 | `None` |
| `startup` | `enum:_is_single_bit` | 21 | `Mixed` | `Compute+Control` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `json.encoder:JSONEncoder.__init__` | 21 | `ObjectManipulator` | `Control+Object` | 0 | 0 | `-` | 2 | `None` |
| `startup` | `re._compiler:_combine_flags` | 21 | `Mixed` | `Compute+Control` | 0 | 0 | `-` | 2 | `None` |
| `setup` | `__main__:add_metric` | 20 | `NumericLoop` | `Compute+Object+Dynamic` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `codecs:IncrementalEncoder.__init__` | 20 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `glob:_ishidden` | 20 | `NumericLoop` | `Compute` | 0 | 0 | `-` | 2 | `None` |
| `startup` | `importlib.machinery:all_suffixes` | 20 | `Mixed` | `Compute+Dynamic` | 0 | 0 | `Exception` | 2 | `None` |
| `setup` | `lib2to3.fixes.fix_imports:alternates` | 20 | `NumericLoop` | `Compute+Dispatch+Dynamic` | 0 | 0 | `-` | 2 | `None` |
| `startup` | `lib2to3.pgen2.tokenize:group` | 20 | `NumericLoop` | `Compute` | 0 | 0 | `-` | 2 | `None` |
| `startup` | `tokenize:group` | 20 | `NumericLoop` | `Compute` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `__main__:med` | 1 | `BranchFSM` | `Control+Object` | 2 | 0 | `-` | 2 | `None` |
| `steady` | `abc:ABCMeta.__instancecheck__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `codecs:BufferedIncrementalDecoder.__init__` | 1 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `codecs:IncrementalDecoder.__init__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `contextlib:ExitStack.__enter__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `contextlib:_BaseExitStack.__init__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `contextlib:_BaseExitStack._push_exit_callback` | 1 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `contextlib:_BaseExitStack.callback` | 1 | `ObjectManipulator` | `Object+Dispatch` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `contextlib:_GeneratorContextManagerBase.__init__` | 1 | `ObjectManipulator` | `Object+Dispatch` | 0 | 0 | `-` | 2 | `None` |
| `startup` | `enum:EnumType._convert_.<locals>.<lambda>` | 1 | `NumericLoop` | `Compute` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `namedtuple_SelectorKey:SelectorKey.__new__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `selectors:BaseSelector.__enter__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `selectors:BaseSelector.__exit__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `selectors:_BaseSelectorImpl.__init__` | 1 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `selectors:_BaseSelectorImpl.close` | 1 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `selectors:_BaseSelectorImpl.get_map` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `selectors:_SelectorMapping.__init__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `selectors:_SelectorMapping.__len__` | 1 | `ObjectManipulator` | `Object` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `statistics:median` | 1 | `NumericLoop` | `Compute+Control` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `subprocess:Popen.__enter__` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `subprocess:Popen._handle_exitstatus` | 1 | `ObjectManipulator` | `Control+Object+Dispatch` | 0 | 0 | `-` | 2 | `None` |
| `steady` | `subprocess:Popen._posix_spawn` | 1 | `ObjectManipulator` | `Control+Object` | 3 | 2 | `-` | 2 | `None` |
| `steady` | `subprocess:Popen.poll` | 1 | `Trivial` | `-` | 0 | 0 | `-` | 4 | `LowRoi` |
| `steady` | `subprocess:_text_encoding` | 1 | `BranchFSM` | `Control+Object+Dispatch+Dynamic` | 1 | 1 | `-` | 2 | `None` |

| 形状读法 | 判断 |
|---|---|
| 高频编译函数几乎都是 `codeB=0` 的小函数 | 当前不是大函数编译风暴；继续按 `codeB>0` 粗拦收益很低。 |
| setup 阶段被编译的两个高频函数都是 `NumericLoop` | 粗暴扩大 setup window 会误伤该放行的小型 compute 函数，已有负收益数据验证。 |
| refactor parse/pattern 热点不在当前编译列表中 | 当前剩余 refactor 差距不是“还有大量 refactor 热点被编译”导致。 |
| `LowRoi` 单次函数仍出现，但数量小 | 属于尾部小账，不是 `2to3` 下一轮主优化点。 |

### 6.3 分表二：不进入 gate 的阶段拆解

这里拆的是 phase timer 中能看到、但不能只靠 `jitVectorcall` gate 次数解释的阶段成本。

#### 6.3.1 口径对照

| 口径 | total | startup/site | import `lib2to3.main` | tool init | refactor file | glue | 用途 |
|---|---:|---:|---:|---:|---:|---:|---|
| CPython 3.14.3 JIT | 549.7 | 75.2 | 53.9 | 128.9 | 289.0 | 2.7 | 对标基线 |
| CinderX no plugin | 516.7 | 75.7 | 53.6 | 126.5 | 258.4 | 2.5 | 证明 CinderX 解释器本身不是差距来源 |
| CinderX plugin, JIT disabled | 527.6 | 112.3 | 26.8 | 128.7 | 257.3 | 2.5 | 只用于看 plugin/frame evaluator 固定成本 |
| force interpret-only | 632.8 | 88.5 | 61.1 | 153.4 | 326.9 | 2.9 | 当前 AutoJIT fast path 理想下限 |
| 当前候选 | 673.3 | 108.1 | 64.9 | 161.9 | 332.3 | 6.1 | 当前主线结果 |

#### 6.3.2 startup/site 细账

| 探针 | no plugin | AutoJIT + provider | 差值 | 说明 |
|---|---:|---:|---:|---|
| `-c pass` | 18.5 | 32.1 | +13.6 | plugin 固定启动税 |
| `-c "import contextlib, glob, json, os, sys, time"` | 33.3 | 57.5 | +24.3 | 固定启动税 + 早期 stdlib import 税 |
| 早期 stdlib import 净税 | 14.7 | 25.4 | +10.7 | 扣除 `-c pass` 后的额外成本 |
| provider off + prelude | - | 310.8 | - | 关闭 import provider 会重新触发 import/prelude 编译风暴 |

| C++ / import 计时点 | 中位数 | 读法 |
|---|---:|---|
| `_cinderx_exec_impl()` | 3.31 | C 扩展 exec 本体，不足以解释全部 startup/site 缺口 |
| `jit::initialize()` | 1.84 | 延迟 backend 初始化最多是 1-2ms 小账 |
| `importtime: _cinderx` | 7.69 | 动态加载、模块 exec、import 机制一起摊出的成本 |
| `importtime: _cinderx_auto` | 8.33 | 包含 `_cinderx` 加载、`cinderjit` 可用化和 provider 安装 |

#### 6.3.3 tool/refactor 细账

| 子阶段 | 当前候选中位数 | 说明 |
|---|---:|---|
| `RefactoringTool.__init__` | 161.9 | tool init 主账 |
| `RefactoringTool.get_fixers` | 113.4 | fixer 加载/构造占 tool init 大头 |
| `get_fixers_from_package` | 1.8 | 包扫描本身不是大头 |
| `RefactoringTool.refactor_file` 合计 | 332.3 | 9 个输入文件处理总账 |
| `RefactoringTool.refactor_string` 合计 | 319.8 | 解析/转换字符串主体 |
| `RefactoringTool.summarize` | 0.2 | 可忽略 |

| 输入文件 | `refactor_file` | `refactor_string` |
|---|---:|---:|
| `urlresolvers.py.txt` | 136.1 | 131.6 |
| `mail.py.txt` | 119.2 | 115.3 |
| `paginator.py.txt` | 34.8 | 32.9 |
| `context_processors.py.txt` | 28.1 | 26.5 |

#### 6.3.4 已排除方向

| 方向 | 数据 | 判断 |
|---|---|---|
| setup/main window 扩大 | 正式 `765ms +- 11ms`，phase `777.7ms`，`forced_compile` 从约 10 增到 77-79 | 负收益，已回滚 |
| refactor parse/pattern 热点放行 | `allow-parse=876.5ms`，`allow-pattern=885.9ms`，`allow-all=1081.0ms` | 热点是真热点，但 CinderX JIT 当前 ROI 为负 |
| refactor 热点 suppress | `suppress-all=670.5ms`，默认 off `669.3ms` | 基本无收益 |
| C 侧 import wrapper | 正式 `673ms +- 2ms`，与当前候选同量级 | 对 `2to3` 无稳定收益，先暂缓 |
| lazy `cinderjit` import | 正式 `675ms +- 2ms`，与当前候选同量级 | 对 `2to3` 无稳定收益，先暂缓 |
| import-depth 内跳过计数 | prelude `57.5ms -> 51.6ms`，但 `2to3` full `669.3ms -> 849.7ms`，`jit_vectorcall` 暴涨到 `607856` | 不能生产化；会让函数长期停在未分类 gate 状态 |
| import 期提前分类冻结 | `-c pass` `32.1ms -> 33.0ms`，prelude `57.5ms -> 59.9ms` | 第一次调用扫字节码，成本抵消 gate 次数收益 |
| structure key scanner 微优化 | miss 扫描 `15.98ms -> 15.05ms` | 只省约 `0.9ms`，不是当前 85% 目标主杠杆 |

### 6.4 当前结论和下一步

| 问题 | 结论 | 下一步 |
|---|---|---|
| `2to3` 开启 CinderX JIT 后为什么慢 | 优化前是全阶段低阈值编译风暴：startup/import/tool/refactor 都被放大。 | 已由 import provider、lowROI、classified warmup return、cold-bit 等策略解决大头。 |
| 当前还差在哪里 | 当前比 CPython JIT 多 `123.6ms`，比 85% 目标线慢约 `26.6ms`；可由 force-interpret 下限解释的空间约 `40.5ms`。 | 先打 startup/site 和 tool init 小账，不再追几百毫秒级编译风暴。 |
| gate 内还有什么可挖 | structure key cache miss 扫描约 `16ms`，但前两个 scanner 微优化只省约 `0.9ms`。 | 除非有更大粒度的缓存/复用方案，否则不优先提交 scanner 微优化。 |
| refactor 本体是否该放行 JIT 热点 | 不该。parse/pattern 是真热点，但放行后 phase 退到 `876-1081ms`。 | 保持当前策略；refactor 差距先当作 CinderX JIT ROI 负的小账，而不是白名单目标。 |
| import/setup 是否需要分开决策 | split-only 诊断字段可保留，但当前没有证明分叉阈值有收益。 | 保留阶段归因能力；只有拿到明确阶段特有形状和 A/B 收益，再改策略。 |

## 7 用例：python_startup

### 7.1 总体判断

`python_startup` 是“开 CinderX JIT 后启动变慢”的固定开销用例，不是 JIT 收益用例。下面的分阶段平铺账本一眼说明：开 CinderX（优化前 `PYTHONJITAUTO=2`）相对 CPython JIT 基线慢在哪、当前 AutoJIT 候选优化掉了多少、还剩多少；后接全量 gate 函数形状表。

- 基线：CPython 3.14.3 JIT，**21.8ms**（site）/ 13.6ms（`no_site`）
- 优化前：CinderX JIT `PYTHONJITAUTO=2`，**234.4ms**
- 优化后：当前 AutoJIT 候选（`auto:2` + `find_and_load`），**36.3ms**
- 继续可挖空间：当前候选对比 force interpret-only（`auto:2097152`，gate 但不编译）**31.8ms** 的差值，表示“AutoJIT 准入/gate 路径理论上还能挤多少”，不保证全部可拿。

口径：同一 `_cinderx.so`、`/opt/python314`、`taskset -c 30`、wall 中位数，只改环境变量；各阶段由 `-X importtime` 交叉 wall 总耗时拆出。wall harness 比 §5 的 pyperf 口径（base 18.1 / 优化前 224.9 / 优化后 38.6ms）高约 3ms 固定开销，但该开销在四口径相同，**下表的差距列（优化前差距/已优化掉/剩余/可挖空间）与 harness 无关**。

### 7.2 分阶段耗时账本

| 耗时项 | CPython JIT 基线 | CinderX JIT 优化前 | 优化前差距 | 当前优化后 | 已优化掉 | 当前剩余差距 | 继续可挖空间 | 下一步判断 |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 解释器核心（pre-site, `-S`） | 13.6ms | 13.7ms | +0.1ms | 13.7ms | 0.0ms | +0.1ms | ~0ms | CPython 自身；plugin 不在 `-S` 下加载，已追平，无可挖 |
| `_cinderx` 插件加载 + JIT init | 0.4ms | 186.0ms | +185.6ms | 8.5ms | 177.4ms | +8.1ms | ~0.5ms | **最大单项**：`PYTHONJITAUTO=2` 在 init 扫描+调度全部已有函数→编译风暴；bc347c29 跳过已有函数扫描=最大收益；剩 dlopen+exec ~5.4ms + `jit::initialize()` ~1.6ms 地板 |
| site/stdlib import + 逐帧/gate 税 | 7.7ms | 34.8ms | +27.1ms | 14.0ms | 20.7ms | +6.3ms | ~4.1ms | per-frame 计数/gate/少量编译摊在 import 上；import 提前分类冻结实测负收益 |
| **合计（site）** | **21.8ms** | **234.4ms** | **+212.6ms** | **36.3ms** | **198.1ms** | **+14.5ms** | **~4.5ms** | 已解决 93%；当前 0.60x，距 CPython JIT 还差 +14.5ms = 插件地板 ~8 + import 运行税 ~6 |

注：`_cinderx` 插件加载列取 `-X importtime` 的 `_cinderx_auto` 累计（编译风暴由 init 期 `schedule_existing_functions` 触发，importtime 归在此项）；核心/合计为 wall 中位数；import 行 = 合计 − 核心 − 插件加载，吸收口径差。

读表三条结论：

- 优化前（`PYTHONJITAUTO=2`）+212.6ms 差距里 **+185.6ms 全在插件加载/JIT init**——低阈值在 init 扫描并调度所有已存在函数，造成编译风暴；不是 benchmark 本体慢。
- bc347c29（跳过已有函数扫描 + lazy bootstrap）**已优化掉 198.1ms（93%）**，几乎全部来自上面这一项。
- 当前剩余 +14.5ms 无干净单点杠杆：插件地板 ~8ms（dlopen+exec ~5.4 不可削、`jit::initialize` ~1.6 可惰性化但太小）+ import 逐帧/gate 运行税 ~6ms（frame evaluator 承重）。继续可挖空间仅 ~4.5ms（来自完全不编译），但 python_startup 本就只编译 1 个函数（见 7.3），且 import 提前分类冻结穿刺为负收益。

### 7.3 全量 gate 函数形状与策略

`python -c pass`（含 site）下进入 AutoJIT gate 的**全部 17 个函数**（`auto:2` + `find_and_load`，shape 日志口径）。绝大多数是 import 机制函数，被正确判为延迟；只有 1 个达阈值被编译。

| 函数 | 形状 | 策略 | 判断 |
|---|---|---|---|
| `_sitebuiltins:_Printer.__init__` | `ObjectManipulator`, dims=`Control+Object`, `loop=2`, `codeB=0`, `risk=None`, `startup=false`, `synthetic=true` | `2/None` | **唯一编译**；site 期小对象构造，`loop=2` 达阈值即编译，非 compile storm 主因 |
| `_frozen_importlib:_verbose_message` | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false`, `synthetic=true` | `1000/LowRoi` | import 调试输出；低 ROI 延迟到 1000，启动期热度不足不编译 |
| `posixpath:join` | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=1`, `risk=Exception`, `startup=false`, `synthetic=true` | `1000/LowRoi` | 路径拼接，带异常边+循环；低 ROI 延迟，启动期不编译 |
| `_frozen_importlib:BuiltinImporter.find_spec` | `BranchFSM`, dims=`Control+Object+Dispatch+Suspend`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | import 查找核心；import 窗口延迟正确 |
| `_frozen_importlib_external:PathFinder.find_spec` | `BranchFSM`, dims=`Control+Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | 路径查找；import 窗口延迟 |
| `_frozen_importlib:ModuleSpec.__init__` | `ObjectManipulator`, dims=`Control+Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | 模块 spec 构造；import 窗口对象构造延迟 |
| `_frozen_importlib:_ModuleLockManager.__exit__` | `ObjectManipulator`, dims=`Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | import 锁管理；旧口径曾编译，现按 startup 窗口延迟 |
| `_frozen_importlib:_WeakValueDictionary.__init__.<locals>.KeyedRef.__new__` | `ReflectionMeta`, dims=`Object+Dispatch+Suspend`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | weakref 分配；import 窗口延迟 |
| `_frozen_importlib:_WeakValueDictionary.__init__.<locals>.KeyedRef.remove` | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=true` | `2097152/StartupInit` | weakref 回调；import 窗口延迟 |
| `_distutils_hack:DistutilsMetaFinder.find_spec` | `CallDispatcher`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=false` | `2097152/StartupInit` | setuptools import hook；import 窗口高成本分发延迟 |
| `_distutils_hack:DistutilsMetaFinder.find_spec.<locals>.<lambda>` | `Trivial`, dims=`-`, `loop=0`, `codeB=0`, `risk=None`, `startup=true`, `synthetic=false` | `2097152/StartupInit` | 启动期 trivial lambda；延迟无害 |
| `_frozen_importlib:_find_and_load` | `BranchFSM`, dims=`Control+Dispatch+Suspend`, `loop=1`, `codeB=1`, `risk=Exception`, `startup=true`, `synthetic=true` | `2097152/RiskDefer` | import 主入口，带异常边+循环；风险延迟，import 窗口核心被保护函数 |
| `importlib._bootstrap:_get_module_lock.<locals>.cb` | `BranchFSM`, dims=`Control+Object+Dispatch+Suspend`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=true`, `synthetic=true` | `2097152/RiskDefer` | import 锁回调带异常边；风险延迟 |
| `_frozen_importlib:_get_module_lock.<locals>.cb` | `BranchFSM`, dims=`Control+Object+Dispatch+Suspend`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=false`, `synthetic=true` | `2097152/RiskDefer` | 同上（另一 import 名空间）；风险延迟 |
| `_frozen_importlib:_WeakValueDictionary.__init__.<locals>.KeyedRef.__init__` | `ReflectionMeta`, dims=`Dispatch+Suspend`, `loop=0`, `codeB=0`, `risk=Dynamic`, `startup=true`, `synthetic=true` | `2097152/RiskDefer` | weakref 反射构造带动态风险；风险延迟 |
| `collections.abc:Mapping.get` | `BranchFSM`, dims=`Control`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=false`, `synthetic=true` | `2097152/RiskDefer` | 带异常边的 `Mapping.get`；风险延迟 |
| `_cinderx_auto:_make_autojit_import_wrapper.<locals>.wrapper` | `BranchFSM`, dims=`Control+Dispatch+Suspend`, `loop=0`, `codeB=1`, `risk=Exception`, `startup=false`, `synthetic=false` | `2097152/RiskDefer` | AutoJIT 自己装的 import provider wrapper；带异常边，风险延迟（不编译自身基础设施） |

全表反证 7.2 的结论：17 个 gate 函数 = **8 StartupInit + 6 RiskDefer + 2 LowRoi + 1 编译（`2/None`）**。它们都是 import 机制小函数（`codeB` 多为 0、`risk` 多为 Exception/None），分类策略把它们全部正确判为延迟——**python_startup 的差距确实不在“编译了什么”，而在 7.2 的固定加载 + 逐帧簿记税**。（被编译集只有 1–2 个，随 `startup_phase` 边界时序略有抖动。）

### 7.4 优化结论

bc347c29（lazy bootstrap + 跳过已有函数扫描）已优化掉 93%（212.6→14.5ms），几乎全部来自插件加载/JIT init 的编译风暴。当前剩余 +14.5ms 无干净单点杠杆：插件地板 ~8ms（dlopen+exec 不可削、`jit::initialize` ~1.6ms 太小）、import 逐帧/gate 税 ~6ms（frame evaluator 承重）。已穿刺否决：A 逐 def tracking 延后（~0.16ms）、native C wrapper（~0.18ms）、跳过逐帧计数 C（硬上界 ≤0.46%，亚噪声）、import 提前分类冻结（负收益）、延迟 `jit::initialize()`（~1.6ms 太小）。唯一未做的真杠杆是 import 窗口更细粒度分类（改动大，需分阶段 compile event 证明），ROI 低于换方向。

## 8 用例：coverage

### 8.1 总体判断

`coverage` 是 JIT 回归观察用例，不是非 JIT 主目标。真实 worker gate 证据显示 benchmark 本体 `__main__:fibonacci` 是 `NumericLoop` 且 `compute=true`，应放行；`coverage` 框架自身会在 steady 阶段编译大量 `coverage.*` 函数，其中有高成本的动态/对象/控制混合函数。不能用 `coverage` 反推出“全局拦 steady highcost”，否则会误伤 `unpack_sequence` 这类核心热点。

### 8.2 摘要

| 指标 | 值 |
|---|---:|
| 编译事件 | 1314 |
| unique compiled | 208 |
| gate 事件 | 110448 |
| unique gated | 608 |

### 8.3 benchmark 本体函数

| 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---:|---|---|---|
| `__main__:fibonacci` | 7 | 8.0ms | `NumericLoop`, dims=`Compute+Control+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None`, `compute=true`, `startup=false` | `2/None` | 放行；这是 benchmark 热点 |
| `__main__:bench_coverage` | 7 | 20.9ms | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `compute=false`, `startup=false` | `2/None` | 放行；驱动函数会反复执行 |

### 8.4 coverage 框架高成本函数

| 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---:|---|---|---|
| `coverage.inorout:InOrOut.__init__` | 7 | 421.4ms | `ReflectionMeta`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=3`, `risk=Dynamic+Exception+HugeCode`, `startup=false` | `2/None` | 高成本但发生在 steady worker；不能由 import-window 策略处理，需单独 ROI 证据 |
| `coverage.control:Coverage._init_for_start` | 7 | 238.5ms | `ObjectManipulator`, dims=`Control+Object`, `loop=1`, `codeB=2`, `risk=HugeCode`, `startup=false` | `2/None` | 高成本 coverage 初始化；若要拦需证明动态收益不足 |
| `coverage.config:read_coverage_config` | 7 | 60.8ms | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=1`, `codeB=1`, `risk=None` | `2/None` | 配置读取；steady 内是否值得编译待单独验证 |
| `coverage.collector:Collector.stop` | 7 | 58.4ms | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=1`, `codeB=1`, `risk=None` | `2/None` | coverage 生命周期函数；不能用 import-window 规则处理 |
| `coverage.control:Coverage._init` | 7 | 57.9ms | `ObjectManipulator`, dims=`Control+Object`, `loop=1`, `codeB=1`, `risk=None` | `2/None` | 框架初始化函数，需 ROI 证据 |
| `coverage.collector:Collector.pause` | 7 | 52.6ms | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | 可能有执行收益，不能静态全拦 |
| `coverage.config:CoverageConfig.post_process` | 7 | 46.4ms | `ObjectManipulator`, dims=`Control+Object`, `loop=2`, `codeB=1`, `risk=Exception` | `2/None` | 异常边成本较高，需动态收益证据 |
| `coverage.inorout:name_for_module` | 7 | 37.2ms | `BranchFSM`, dims=`Control`, `loop=3`, `codeB=1`, `risk=None` | `2/None` | 分支循环，可能收益；不应粗暴拦截 |
| `coverage.plugin_support:Plugins.load_plugins` | 7 | 31.7ms | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=1`, `codeB=1`, `risk=None` | `2/None` | 插件加载，若在 startup/import 可延迟；此处是 steady worker |
| `coverage.files:TreeMatcher.match` | 7 | 26.6ms | `BranchFSM`, dims=`Control+Object`, `loop=2`, `codeB=0`, `risk=None` | `2/None` | 小而热的匹配函数，放行 |

### 8.5 startup=true 样本

| 函数 | 形状 | 策略 | 判断 |
|---|---|---|---|
| `coverage.misc:isolate_module` | `ReflectionMeta`, dims=`Control+Object+Dispatch+Dynamic`, `loop=1`, `codeB=1`, `risk=None`, `startup=true` | `2097152/StartupInit` | import-window 高成本非数值，延迟正确 |
| `coverage.sqldata:_locked` | `ReflectionMeta`, dims=`Object+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=Dynamic`, `startup=true` | `2097152/RiskDefer` | 风险延迟正确 |
| `coverage.misc:expensive` | `ReflectionMeta`, dims=`Control+Object+Dynamic`, `loop=0`, `codeB=0`, `risk=Dynamic`, `startup=true` | `2097152/RiskDefer` | 风险延迟正确 |

## 9 用例：generators

### 9.1 总体判断

`generators` 是 JIT 用例误伤检查样本。它的核心收益来自递归构造和 generator 遍历，尤其 `Tree.__iter__`。最新回归证明：`Tree.__iter__` 的 `yield from` 会生成 `CLEANUP_THROW/RERAISE`，分类器看到 `risk=Exception`，但这是 generator 协议清理路径，不是业务异常、动态派发或 deopt 负 ROI 证据。把普通 generator 按 `RiskDefer/interpret-only` 冻结会直接把 `generators` 从约 `21.7ms` 打到 `60.2ms`。

当前策略边界：steady-state 普通 generator 只要风险位没有超出 `Suspend|Exception`，恢复全局阈值；startup window、static code、coroutine/async 状态机、`Dynamic/HugeCode` 等其它风险仍按原策略延迟。这个结论很重要：**suspendable 可以延迟，但不能把普通 generator 的 cleanup exception 当成禁编证据。**

### 9.2 函数形状表

| 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---:|---|---|---|
| `__main__:Tree.__iter__` | 7 | 35.4ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=0`, `risk=Exception`, `suspend=true`, `startup=false`, `compute=false` | `2/None`（plain generator steady-state override） | 必须放行；`risk=Exception` 来自 `yield from` cleanup，破损策略冻结它会导致 `generators` 2.78x slower |
| `__main__:bench_generators` | 7 | 27.7ms | `CallDispatcher`, dims=`Control+Dispatch+Dynamic`, `loop=2`, `codeB=1`, `risk=None`, `startup=false` | `2/None` | 放行；驱动循环有热度 |
| `__main__:tree` | 7 | 21.6ms | `NumericLoop`, dims=`Compute+Control+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None`, `compute=true`, `startup=false` | `2/None` | 放行；compute 提示明确 |
| `__main__:Tree.__init__` | 7 | 4.8ms | `ObjectManipulator`, dims=`Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | 小对象构造，放行 |

### 9.3 2026-06-10 误伤修复证据

| 口径 | `Tree.__iter__` 是否编译 | `generators` 结果 | 读法 |
|---|---|---:|---|
| `PYTHONJITAUTO=2` | 是，after one probe `True` | `21.7ms +- 6.2ms` | CinderX JIT 原始低阈值会编译核心 generator 热点 |
| 破损 AutoJIT | 否，after one probe `False`，count 停在 2 | `60.2ms +- 0.3ms` | `Tree.__iter__` 被解释冻结，核心收益丢失 |
| 修复后 AutoJIT | 是，compile-events 显示 `limit=2 reason=None` | `21.7ms +- 5.8ms` | 恢复到 `auto=2` 水平；相对破损 `2.78x faster`，相对 `auto=2` 不显著 |

同轮用 `2to3` 做误伤检查：同一容器、同一 `/opt/python314`、同一 `CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 PYTHONJITHUGEPAGES=0 --warmup 3 --affinity=30`，只切换 generator override。改动前 `578ms +- 1ms`，改动后 `579ms +- 5ms`，`pyperf compare_to` 不显著。结论：plain generator steady-state 放行修复了 `generators` 误伤，未测出 `2to3` 回归。

## 10 用例：unpack_sequence

### 10.1 总体判断

`unpack_sequence` 是“不要全局拦 highcost”的关键反例。核心函数 `do_unpacking` 是纯 `ObjectManipulator`，`codeB=3`、`risk=HugeCode`，但它正是 benchmark 热点；如果按 highcost 全局延迟或禁止，会直接误伤。当前策略只在 `startup=true` 的 import window 内延迟高成本非数值形状，steady-state 的 `do_unpacking` 保持 `2/None`，这是正确边界。

### 10.2 函数形状表

| 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---:|---|---|---|
| `__main__:do_unpacking` | 7 | 26140.5ms | `ObjectManipulator`, dims=`Object`, `loop=1`, `codeB=3`, `risk=HugeCode`, `suspend=false`, `startup=false`, `compute=false` | `2/None` | 必须放行；这是核心热点，不能被 steady highcost 策略拦截 |
| `__main__:bench_all` | 0 | 0.0ms | `CallDispatcher`, dims=`Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `1000/LowRoi` | 不编译可接受；驱动分发不是核心收益点 |
| `__main__:bench_tuple_unpacking` | 0 | 0.0ms | `Mixed(Dispatch+Dynamic)`, dims=`Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=Dynamic`, `startup=false` | `2097152/RiskDefer` | 风险延迟正确；非核心热点 |
| `__main__:bench_list_unpacking` | 0 | 0.0ms | `Mixed(Dispatch+Dynamic)`, dims=`Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=Dynamic`, `startup=false` | `2097152/RiskDefer` | 风险延迟正确；非核心热点 |

## 11 用例：sqlalchemy 系列

### 11.1 总体判断

`sqlalchemy_declarative` 是 ORM declarative class 构建、mapper 配置、SQL 编译、unit of work flush/commit 混合在一起的框架型 steady workload。它不是 startup/import 编译风暴：当前 debug worker 的编译事件里 `2099/2134` 发生在 steady，import 只有 `35`。

当前 AutoJIT 分类确实让这个用例的 **mean** 明显劣化，但不是每个 measured value 都慢。正式 `warmup=3` 结果呈双峰：低位值约 `364-382ms`，比 `PYTHONJITAUTO=2` 的 `372-398ms` 还好；慢值固定冲到 `458-626ms`，把均值拉到 `434.4ms`。这说明当前策略有两个问题叠加：

- `LowRoi=1000` 把一批函数的编译延后，但没有禁止编译；编译债溢出到 pyperf measured values，形成长尾。
- 即使把长尾部分消化掉，CinderX JIT 本身对 SQLAlchemy ORM steady 路径仍是负 ROI；plugin-no-JIT 已经快于 CPython JIT。

### 11.2 总表：差距账本

口径：`blue-98`，`--warmup 3 --affinity=30`，正式 pyperformance 60 values。CPython JIT 使用 `/opt/python314-jit/bin/python3.14`；CinderX 口径使用同一 `/opt/python314` 和同一 SQLAlchemy venv，只改变 `CINDERX_PLUGIN_ENABLE` / `PYTHONJITAUTO` / provider 环境变量。

基线：CPython 3.14.3 JIT **269.2ms**，85% 目标线约 **316.8ms**。优化前：CinderX `PYTHONJITAUTO=2` **387.8ms**，慢 **+118.6ms**。当前 AutoJIT：`auto:2 + provider` **434.4ms mean / 377.5ms median**，按 mean 慢 **+165.2ms**。

| 成本项 | 取数方式 | 优化前差距 | 当前剩余差距 | 已优化掉 | 继续可挖空间 | 下一步判断 |
|---|---|---:|---:|---:|---:|---|
| CinderX 解释器基底 | CinderX no-plugin `245.5ms` - CPython JIT `269.2ms` | -23.7ms | -23.7ms | 0.0ms | 0.0ms | 不是瓶颈；不开插件时 CinderX 路径更快 |
| 插件固定加载/初始化 | CinderX plugin-no-JIT `249.8ms` - CinderX no-plugin `245.5ms` | +4.3ms | +4.3ms | 0.0ms | 小 | 插件固定成本很小，plugin-no-JIT 已快于 CPython JIT |
| CinderX JIT 动态成本 | `PYTHONJITAUTO=2` - plugin-no-JIT | +138.0ms | +138.0ms | 0.0ms | ~138ms | steady ORM/SQL 编译/unit-of-work 路径进入 JIT 后整体负 ROI |
| AutoJIT 延迟编译长尾 | 当前 AutoJIT mean - `PYTHONJITAUTO=2` | 0.0ms | +46.6ms | -46.6ms | ~46.6ms | `LowRoi=1000` 编译债溢出 measured values；应专项处理 delayed compile，而不是继续调 import provider |
| import/startup 编译风暴 | debug worker 编译事件：import `35`，steady `2099` | 非主项 | 非主项 | 0.0ms | 不作为主线 | import 只占 `35/2134` 个 worker 编译事件 |
| **合计** | 总耗时减 CPython JIT | **+118.6ms** | **+165.2ms** | **-46.6ms** | **~184.6ms 到 plugin-no-JIT** | 当前 AutoJIT 低位值有改善，但 mean 被延迟编译长尾拖垮；根问题仍是 SQLAlchemy 对当前 CinderX JIT 动态负 ROI |

读表三条结论：

- `sqlalchemy_declarative` 不需要 CinderX JIT 才能达标：plugin-no-JIT `249.8ms`，已经优于 CPython JIT `269.2ms` 和 85% 目标线 `316.8ms`。
- 当前 AutoJIT 比 `auto=2` 多慢 `46.6ms`，主因不是多编译 import，而是 `LowRoi=1000` 推迟编译后在测量窗口内还会继续编译。
- 即使拿回这 `46.6ms`，`auto=2` 本身仍有 `+138.0ms` JIT 动态成本；SQLAlchemy 后续优化应走 deopt/guard/expected-exception 或 deopt-aware 禁编，不应靠 startup/import 泛化。

### 11.3 分表一：双峰与 warmup 证据

| 口径 | mean | median | stdev | p75 | p90 | max | 判断 |
|---|---:|---:|---:|---:|---:|---:|---|
| `PYTHONJITAUTO=2`，`warmup=3` | 387.8ms | 389.4ms | 5.7ms | 392.0ms | 394.7ms | 398.1ms | 慢，但分布稳定 |
| 当前 AutoJIT，`warmup=3` | 434.4ms | 377.5ms | 89.1ms | 475.5ms | 567.9ms | 625.7ms | 双峰长尾；mean 严重劣化，median 略好于 `auto=2` |
| 当前 AutoJIT，`warmup=10 --fast` | 386.3ms | 368.5ms | 41.9ms | 369.5ms | 476.4ms | 478.6ms | 长尾明显收敛，但仍有首个 measured value 偏慢；只作机制验证，不替代正式口径 |

正式 `warmup=3` 的当前 AutoJIT 每个 pyperf run 都有固定位置慢值，例如第一组 values 为 `616.5, 457.9, 559.4, 366.4, 365.9, 365.7, 380.7, 474.1, 365.9, 364.9ms`。这不是随机噪声：低位平台说明延迟策略能减少一部分无收益 JIT，慢位说明延迟后仍然发生的编译/状态切换进入了 measured values。

### 11.4 分表二：gate 与编译规模

debug 口径：当前 AutoJIT，`--fast --warmup 1`，只用于函数形状和路径计数，不作为性能数值。过滤掉 pip/venv 准备进程后，5 个 benchmark worker 合计：

| gate 路径 | 次数 | 含义 |
|---|---:|---|
| `jit_vectorcall` | 403291 | 进入 AutoJIT gate 的总次数 |
| `global_threshold_return` | 17305 | 还没到全局阈值，返回解释执行 |
| `classified_warmup_return` | 358240 | 已分类，但 `LowRoi` 等策略要求继续解释等待更高热度 |
| `classified_defer_freeze` | 25619 | 判定延迟/解释执行，并恢复 interpreted vectorcall |
| `forced_compile` / `forced_compile_ok` | 2127 / 2127 | 仍然进入 CinderX JIT 编译的次数 |
| `forced_compile_fallback` | 0 | 没有编译失败回退 |

worker 编译事件按阶段和策略分布：

| 维度 | 分布 | 读法 |
|---|---|---|
| 阶段 | `steady=2099`，`import=35` | SQLAlchemy declarative 的 JIT 成本几乎全在 steady 阶段 |
| 策略原因 | `None=1450`，`LowRoi=684` | `LowRoi` 不是禁编；热度足够后仍会编译，形成 delayed compile 债 |
| family | `ObjectManipulator=885`，`BranchFSM=745`，`Trivial=364`，`Mixed=69`，`CallDispatcher=28`，`NumericLoop=26`，`ReflectionMeta=17` | 主体是对象/控制/ORM 框架形状，不是数值循环 |
| unique compiled | 516 | 编译面宽，单一 family 粗拦风险高 |

代表性 SQLAlchemy 编译函数：

| 函数 | 编译事件 | 阶段 | 形状 | 策略 | 判断 |
|---|---:|---|---|---|---|
| `<invalid>:_generated_copy_internals_traversal` | 12 | steady | `ObjectManipulator`, dims=`Object`, `loop=0`, `codeB=0`, `risk=-` | `2/None` | SQLAlchemy 生成的 traversal helper；小函数但数量多 |
| `<invalid>:_generated_cache_key_traversal` | 12 | steady | `ObjectManipulator`, dims=`Object`, `loop=0`, `codeB=0`, `risk=-` | `2/None` | cache key 生成 helper，后续 deopt 也集中在 cache key 路径 |
| `sqlalchemy.orm.mapper:Mapper._configure_pks.<locals>.<genexpr>` | 10 | steady | `BranchFSM`, dims=`Control`, `loop=2`, `codeB=0`, `risk=-` | `2/None` | declarative mapper 配置路径 |
| `sqlalchemy.sql.compiler:DDLCompiler.create_table_constraints.<locals>.<genexpr>` | 10 | steady | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=0`, `risk=-` | `2/None` | SQL/DDL 构造路径 |
| `sqlalchemy.sql.compiler:SQLCompiler._bind_processors.<locals>.<genexpr>` | 8 | steady | `ObjectManipulator`, dims=`Control+Object+Dispatch+Dynamic`, `loop=2`, `codeB=1`, `risk=-` | `2/None` | SQL compiler 绑定处理器 |
| `sqlalchemy.util.langhelpers:format_argspec_plus` | 5 | import | `NumericLoop`, dims=`Compute+Control`, `loop=0`, `codeB=2`, `risk=HugeCode` | `2/None` | 启动期但 compute=true；不能被 startup 非数值规则误伤 |
| `sqlalchemy.engine.default:DefaultExecutionContext._init_statement` | 5 | steady | `ObjectManipulator`, dims=`Control+Object`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode` | `2/None` | engine 执行上下文核心路径，直接放行仍可能负 ROI |
| `sqlalchemy.sql.traversals:HasCacheKey._gen_cache_key` | 4 | steady | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode` | `1000/LowRoi` | cache key 热路径；延迟后仍编译，且 deopt 高 |
| `sqlalchemy.sql.coercions:expect` | 4 | steady | `BranchFSM`, dims=`Control+Object`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode` | `1000/LowRoi` | SQL coercion 多态路径，deopt 高 |
| `sqlalchemy.orm.session:Session._flush` | 4 | steady | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode` | `1000/LowRoi` | ORM flush 核心路径；不能 import-only 粗拦 |
| `sqlalchemy.orm.persistence:_emit_insert_statements` | 4 | steady | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode` | `1000/LowRoi` | ORM insert 核心路径；当前延迟到热后仍编译 |
| `sqlalchemy.orm.loading:_instance_processor` | 4 | steady | `ReflectionMeta`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode` | `2/None` | 高成本反射/对象处理，直接放行但动态 ROI 可疑 |

### 11.5 分表三：JIT 动态成本

`PYTHONJITDUMPSTATS=1` 的 debug worker 统计显示，SQLAlchemy declarative 的 deopt 数量没有 dask 那么大，但集中在 ORM/SQL 核心路径，足以解释 `auto=2` 相对 plugin-no-JIT 的 `+138.0ms` 动态成本。

| deopt 类别 | 次数 | 占比 | 读法 |
|---|---:|---:|---|
| `GuardFailure` | 21714 | 98.1% | SQLAlchemy 的 mapper、event、cache key、attribute、row 类型高度多态，JIT guard 经常失效 |
| `UnhandledException` | 402 | 1.8% | pool connect 等路径存在 JIT 未覆盖慢路径 |
| `Raise` | 17 | 0.1% | 少量正常控制流 |
| **合计** | **22133** | **100%** | 动态成本集中在 steady ORM/SQL 路径 |

top deopt site：

| 函数 | 次数 | 原因 | 说明 |
|---|---:|---|---|
| `sqlalchemy.sql.traversals:HasCacheKey._gen_cache_key` | 4616 | `GuardFailure/LOAD_ATTR_SLOT` + `GuardType` | cache key 生成对象形态多态；同时有属性 slot 和类型 guard 失败 |
| `sqlalchemy.orm.unitofwork:UOWTransaction.execute.<locals>.<lambda>` | 3498 | `GuardFailure/LOAD_ATTR_SLOT` | unit-of-work action 类型多态 |
| `sqlalchemy.event.base:_Dispatch._for_instance` | 2162 | `GuardFailure/LOAD_ATTR_SLOT` | event dispatch target 类型多态 |
| `sqlalchemy.event.base:_Dispatch._for_class` | 2135 | `GuardFailure/LOAD_ATTR_SLOT` | class-level dispatch 类型多态 |
| `sqlalchemy.sql.coercions:expect` | 1890 | `GuardFailure/LOAD_ATTR_SLOT` | SQL expression coercion 多态 |
| `sqlalchemy.orm.attributes:AttributeImpl.get` | 1600 | `GuardFailure/LOAD_ATTR_SLOT` | ORM attribute impl 多态 |
| `sqlalchemy.engine.row:BaseRow.__init__` | 1399 | `GuardFailure/GuardType` | row 构造类型 guard 失败 |
| `sqlalchemy.orm.state:InstanceState._expire` | 1393 | `GuardFailure/LOAD_ATTR_SLOT` | instance state/attribute impl 多态 |
| `sqlalchemy.sql.annotation:Annotated.__new__` | 749 | `GuardFailure/LOAD_ATTR_SLOT` | annotated SQL element 多态 |
| `sqlalchemy.sql.elements:ClauseElement._clone` | 700 | `GuardFailure/LOAD_ATTR_SLOT` | SQL AST clone 多态 |
| `sqlalchemy.pool.impl:SingletonThreadPool.connect` | 402 | `UnhandledException/LoadMethodCached` | pool connect 慢路径/异常路径 |

### 11.6 策略判断

| 观察 | 结论 |
|---|---|
| plugin-no-JIT `249.8ms` 已优于 CPython JIT `269.2ms` | SQLAlchemy declarative 不需要 CinderX JIT 才能达标；CinderX JIT 是净负担 |
| worker 编译事件 `2099/2134` 在 steady | 不是 startup/import 问题，不应扩大 provider 或 import-window 策略 |
| `classified_warmup_return=358240`，但仍有 `2127` 次 forced compile | `LowRoi=1000` 是“延迟编译”，不是“避免编译”；在 pyperf `warmup=3` 下会把编译债推入 measured values |
| 当前 AutoJIT median 好于 `auto=2`，mean 差很多 | 分类方向并非完全错误；问题是延迟后仍编译造成长尾，需要把部分形状从“晚编”改成“严格不编/动态禁编” |
| deopt 主要是 SQLAlchemy cache key、event dispatch、unit-of-work、attribute/path 多态 | 下一步应做 deopt-aware 负 ROI 策略：优先验证 top deopt 函数禁编，或在高 deopt 形状上 freeze，而不是继续调静态 family 粗规则 |
| `format_argspec_plus` 是 import 阶段 `compute=true` 高成本函数 | startup/import 策略仍必须保留 compute 保护；不能因 SQLAlchemy 回归而扩大 import 非数值拦截面 |

### 11.7 历史穿刺对照

2026-06-08 的 SQLAlchemy 系列穿刺已经证明：单纯扩大静态 highcost 形状延迟只能拿到小收益，不能把本组用例拉回 CPython JIT 85% 线。

| 口径 | `sqlalchemy_declarative` | `sqlalchemy_imperative` | 判断 |
|---|---:|---:|---|
| CPython 3.14.3 JIT 基线 | 271ms | 47.1ms | 对标基线 |
| 85% 目标线 | 319ms | 55.4ms | CinderX 至少要达到该水平 |
| CinderX plugin，JIT disabled | 245ms | 40.9ms | 已超过目标，说明本用例在 CinderX JIT 下主要问题是负 ROI |
| CinderX `PYTHONJITAUTO=2` | 388ms | 73.7ms | 相对 CPython JIT 几何均值约 1.50x slower |
| 穿刺 v1：只延迟大 `BranchFSM + Control/Object` | 382ms | 70.7ms | 只有小收益，未达目标 |
| 穿刺 v4：扩大到大 `BranchFSM/ObjectManipulator/ReflectionMeta/Mixed + Control/Object` | 375ms | 68.3ms | 相对 `auto=2` 约 1.06x faster；仍相对 CPython JIT 约 1.42x slower |

v1/v4 与本次当前 AutoJIT 结果一致：延迟一部分 ORM highcost 函数可以降低低位平台，但如果这些函数最终仍然编译，或者其它 ORM 多态路径继续进 JIT，整体 mean 仍会被动态成本和延迟编译长尾拖垮。

## 12 用例：sqlglot_v2 系列

### 12.1 总体判断

`sqlglot_v2` 系列是 SQL 解析、转写和优化用例。它的主体工作是 tokenizer、parser、AST 遍历和 optimizer tree rewrite。真实 worker gate 证据显示，`BranchFSM` 是 gate 主体，但 `ObjectManipulator`、`ReflectionMeta`、`Mixed` 也会出现在真正的 parser/optimizer 热路径里。

这组样本说明：`sqlglot` 不是简单的 import 风暴。`startup=true` 小函数可以按 import window 判断，但 parser/optimizer 中 `startup=false` 的高成本对象/分支函数必须看动态收益，不能被“高成本非数值”全局规则粗拦。

### 12.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `sqlglot_v2` | 959 | 243 | 83 | 155660 | 627 | `BranchFSM:102674`, `ReflectionMeta:22015`, `Mixed:15979`, `CallDispatcher:13791` | `LowRoi:131101`, `RiskDefer:23755`, `None:634`, `StartupInit:170` |
| `sqlglot_v2_parse` | 903 | 226 | 69 | 147500 | 595 | `BranchFSM:101218`, `Mixed:21151`, `CallDispatcher:18173`, `ReflectionMeta:5738` | `LowRoi:142875`, `RiskDefer:3860`, `None:595`, `StartupInit:170` |
| `sqlglot_v2_transpile` | 1059 | 266 | 104 | 207035 | 664 | `BranchFSM:134311`, `Mixed:26335`, `CallDispatcher:26027`, `ReflectionMeta:19013` | `LowRoi:202400`, `RiskDefer:3783`, `None:682`, `StartupInit:170` |
| `sqlglot_v2_optimize` | 1327 | 330 | 169 | 194910 | 784 | `BranchFSM:139731`, `ReflectionMeta:20550`, `Mixed:16675`, `CallDispatcher:16462` | `LowRoi:181887`, `RiskDefer:11934`, `None:919`, `StartupInit:170` |

### 12.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `sqlglot_v2` | `sqlglot.optimizer.simplify:absorb_and_eliminate` | 4 | 73.3ms | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=HugeCode`, `startup=false` | `1000/LowRoi` | optimizer tree rewrite，高成本分支/对象路径；延迟到高热度合理 |
| `sqlglot_v2` | `sqlglot.optimizer.simplify:uniq_sort` | 4 | 44.4ms | `ReflectionMeta`, dims=`Control+Object+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | optimizer 热路径；不能按反射/动态维度全局拦 |
| `sqlglot_v2` | `sqlglot.expressions:Expression.bfs` | 4 | 40.9ms | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=1`, `risk=None`, `suspend=true`, `startup=false` | `2/None` | AST 遍历热点；suspendable 不应一刀切禁止 |
| `sqlglot_v2` | `__main__:bench_normalize` | 4 | 12.4ms | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |
| `sqlglot_v2_parse` | `sqlglot.tokens:Tokenizer._scan_keywords` | 4 | 67.3ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `1000/LowRoi` | tokenizer 核心路径；低 ROI 延迟，但热度足够时可编译 |
| `sqlglot_v2_parse` | `sqlglot.parser:Parser._parse` | 4 | 45.1ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | parser 核心路径；不能全局延迟 `ObjectManipulator + codeB=2` |
| `sqlglot_v2_parse` | `__main__:bench_parse` | 4 | 11.3ms | `CallDispatcher`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |
| `sqlglot_v2_transpile` | `sqlglot.generator:Generator.select_sql` | 4 | 57.4ms | `ObjectManipulator`, dims=`Object+Dispatch`, `loop=2`, `codeB=2`, `risk=Exception`, `startup=false` | `2/None` | SQL 生成核心路径；全局 highcost 延迟可能误伤 |
| `sqlglot_v2_transpile` | `sqlglot.parser:Parser.validate_expression` | 4 | 46.6ms | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `1000/LowRoi` | 解析校验路径；延迟到高热度合理 |
| `sqlglot_v2_transpile` | `__main__:bench_transpile` | 4 | 10.4ms | `CallDispatcher`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |
| `sqlglot_v2_optimize` | `sqlglot.optimizer.pushdown_projections:pushdown_projections` | 4 | 88.9ms | `ObjectManipulator`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | optimizer 核心热点；如果全局拦 `ObjectManipulator + codeB=2` 会直接误伤 |
| `sqlglot_v2_optimize` | `sqlglot.optimizer.optimize_joins:optimize_joins` | 4 | 64.0ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | optimizer 核心热点，应靠动态收益判断 |
| `sqlglot_v2_optimize` | `__main__:bench_optimize` | 4 | 10.8ms | `CallDispatcher`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |

### 12.4 策略判断

| 观察 | 结论 |
|---|---|
| `sqlglot` 主体函数多为 `startup=false` 的 parser/tokenizer/optimizer | 它主要支撑 steady-state ROI 判断，不应用来扩大 import-window 拦截范围 |
| 多个 `BranchFSM + codeB=2` 函数被 `1000/LowRoi` 延迟 | 当前“低 ROI 延迟到高热度”方向合理；这不是禁编，而是让热度证明收益 |
| 多个 `ObjectManipulator + codeB=2` optimizer/parser 函数仍是 `2/None` | 全局 highcost 延迟会误伤 `sqlglot`；import-window 规则必须绑定 `startup=true` |
| `Expression.bfs` 是 `suspend=true` 但 `limit=2` | suspendable 只能作为风险信号，不能单独决定禁止编译 |
| `new_trie`、`Tokenizer.reset` 等 startup 样本为小函数且 `limit=2` | startup/import 不是一看到 `startup=true` 就拦；核心条件仍是高成本、非数值、非 compute |

## 13 用例：logging 系列

### 13.1 总体判断

`logging` 系列分三类：`silent` 是关闭 debug 日志时的极小热路径，`simple` 和 `format` 是实际输出日志时的对象构造、调用链和格式化路径。2026-06-11 复核后，`logging_silent` 的结论发生反转：**不是编译得不够，而是编译了不该编译的微路径**。

`logging_silent` 的热路径是 `bench_silent -> Logger.debug -> Logger.isEnabledFor(DEBUG)`。HIR/LIR 显示：`Logger.isEnabledFor` 编译后仍然走 `LoadAttrCached + PyObject_IsTrue + PyObject_GetItem` 这类通用 C API 路径；`bench_silent` 编译后只是把 10 次 `logger.debug()` 变成 10 组 `LoadMethodCached + CallMethod`，没有内联 `Logger.debug/isEnabledFor`。因此这两个函数进入 CinderX JIT 都是负收益。

因此 `logging` 支撑两个更窄的策略边界：

- `BranchFSM + risk=Exception` 的 cached predicate 不应因为“看起来像 cache-hit 快路径”就静态放行；没有更强 codegen 支撑时，`Logger.isEnabledFor` 保持 `RiskDefer` 更快。
- `CallDispatcher + Object|Dispatch + loop=1 + codeB=1 + risk=None` 的 call-only loop，如果只是重复方法调用且没有可内联/可优化的主体工作，应按 `LowRoi` 解释执行，避免把调用 trampoline 成本放大到纳秒级微路径里。

### 13.2 摘要

口径：`blue-98`，`--warmup 3 --affinity=30`，正式 pyperformance `logging` 组；CPython JIT 使用 `/opt/python314-jit/bin/python3.14`，CinderX 使用 `CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 CINDERX_AUTOJIT_ROI_BACKOFF=1`。

| 口径 | `logging_format` | `logging_silent` | `logging_simple` | 结论 |
|---|---:|---:|---:|---|
| CPython 3.14.3 JIT | 13.2us | 219ns | 12.0us | 对比基线 |
| 原 AutoJIT | 16.7us | 328ns | 14.8us | `silent` 只有 CPython JIT 的约 67%，明显不达标 |
| 只撤销 cached-predicate 放行 | 15.9us | 271ns | 14.4us | `silent` 提升 1.21x，但仍只有约 81% |
| 撤销 cached-predicate + defer call-only loop | 16.0us | 212ns | 14.3us | `silent` 比 CPython JIT 快 1.03x；`format/simple` 没有显著回归 |

产物：

- 原五口径账本：`blue-98:/results/autojit-logging-ledger-20260611_180634`
- 只撤销 cached-predicate：`blue-98:/results/autojit-logging-formal-after-cached-predicate-defer-20260611_194336`
- 最终 call-only loop 策略：`blue-98:/results/autojit-logging-formal-after-callonly-defer-20260611_195041`

### 13.3 代表函数形状表

| 用例 | 函数 | 旧策略 | 新策略 | 形状 | 证据 | 判断 |
|---|---|---|---|---|---|---|
| `logging_silent` | `__main__:bench_silent` | `2/None`，编译 | `2097152/LowRoi`，解释执行 | `CallDispatcher`, dims=`Object+Dispatch`, `loop=1`, `codeB=1`, `risk=None`, `startup=false` | JIT-list 穿刺：只允许它编译时 `323ns -> 257ns`；禁掉所有 logging target 时 `212ns` | 外层只是 10 次方法调用，JIT 无法内联核心路径，编译负收益 |
| `logging_silent` | `logging:Logger.isEnabledFor` | cached-predicate 例外放行 | `2097152/RiskDefer`，解释执行 | `BranchFSM`, dims=`Control+Object`, `loop=0`, `codeB=1`, `risk=Exception`, `startup=false` | HIR/LIR 显示仍是通用 attr/subscript C API；撤销放行后正式 `328ns -> 271ns` | cache miss 异常边不能静态证明 JIT 正收益，保持 RiskDefer |
| `logging_silent` | `logging:Logger.debug` | 未作为 AutoJIT 主编译对象 | 不变 | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `auto=2` 会编译更多函数且整体更慢；AutoJIT compile-events 中该函数不是剩余主因 | 暂不为它加特例；先避免外层和 predicate 负收益 |
| `logging_simple` | `logging:LogRecord.__init__` | `1000/LowRoi` | 不变 | `BranchFSM`, dims=`Control+Object`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | 最终正式 `logging_simple=14.3us`，相对原 AutoJIT 1.04x faster | 输出路径不是本次问题；LowRoi 延迟仍合理 |
| `logging_simple` | `__main__:bench_simple_output` | `2/None` | codeB=2，不命中 call-only loop 新规则 | `CallDispatcher`, dims=`Object+Dispatch`, `loop=1`, `codeB=2`, `risk=None`, `startup=false` | 最终正式 `logging_simple` 无回归 | 只拦 codeB=1 的极小 call-only loop，避免误伤输出路径 |

### 13.4 策略判断

| 观察 | 结论 |
|---|---|
| `silent` 的外层 benchmark 本体虽有 loop，但主体是重复方法调用 | 没有内联/专用 codegen 前，call-only dispatch loop 编译负收益；应按 LowRoi 解释执行 |
| `Logger.isEnabledFor` 的异常风险来自 cache miss/fill，但 JIT 后仍走通用 C API | 不能只因 cache-hit 直觉就放行 cached predicate；保持 RiskDefer 更快 |
| `simple/format` 中多个输出核心函数是 `startup=false + LowRoi + gate_count≈3996` | `LowRoi` 是热度延迟，不是禁编；热日志路径仍能进入 JIT |
| `StreamHandler.emit/flush` 等异常边函数仍被 `RiskDefer` 延迟 | 风险延迟对异常边丰富的 logging 框架仍有保护作用；不要用 `Logger.isEnabledFor` 反推全局放宽 |
| logging top 编译耗时里有 `importlib.metadata`、`argparse` 等 driver/setup 噪声 | 调 logging 策略时必须看 benchmark-self，不能只看全局 top 编译耗时 |

### 13.5 2026-06-11 复核记录

`logging_silent` 的 disabled debug 路径为 `bench_silent -> Logger.debug -> Logger.isEnabledFor(DEBUG)`；当 logger level 为 `WARNING` 时不会进入 `_log`。本轮用三组证据修正旧判断：

- 五口径账本：CPython JIT `219ns`、CinderX no-plugin `211ns`、plugin-no-JIT `211ns`、`auto=2` `367ns`、原 AutoJIT `328ns`。差距只在启用 JIT 后出现。
- HIR/LIR：`bench_silent` 是 10 组 `LoadMethodCached + CallMethod`；`Logger.isEnabledFor` 是 `LoadAttrCached + IsTruthy + BinaryOp<Subscript>`，没有把 cache-hit 路径降成比解释器更快的专用路径。
- JIT-list 穿刺：正常 AutoJIT `323ns`；只允许 `bench_silent` 编译为 `257ns`；禁止 logging target 编译为 `212ns`。因此两个函数都负收益，其中 `isEnabledFor` 约贡献第一段改善，call-only loop 约贡献第二段改善。

生产实现不新增 logging 白名单、不新增 `StructureKey` 位，只调整两个通用策略：

- 删除 cached-predicate 的 `RiskDefer -> None` 静态放行，`Logger.isEnabledFor` 回到解释执行。
- 新增 `CallDispatcher + dims=Object|Dispatch + loop=1 + codeB=1 + risk=None + startup=false` 的 `LowRoi` 延迟，覆盖 `bench_silent` 这类极小 call-only loop。

验证：新增 `test_plugin_defers_logging_disabled_fast_path` 和 `test_plugin_defers_call_only_dispatch_loop`。正式 `logging` 组最终为 `logging_silent=212ns`，相对原 AutoJIT `328ns` 为 `1.55x faster`，相对 CPython JIT `219ns` 为 `1.03x faster`。

## 14 用例：sympy 系列

### 14.1 总体判断

`sympy` 系列是符号计算 workload，四个子场景分别覆盖展开、积分、求和和字符串化。它的热路径大量是 `BranchFSM`、`ObjectManipulator`、`ReflectionMeta`、`Mixed`，而且不少函数 `codeB=2/3`、`risk=HugeCode/Exception`。这些不是 import 噪声，而是符号代数、表达式树重写、多项式和打印的主体工作。

因此 `sympy` 是“不能全局拦 highcost”的强样本。它也说明 `compute=true` 不只出现在数值 benchmark：符号计算里也有 `Compute+Control` 大函数，当前有的被 `LowRoi` 延迟到 1000 后仍会编译，有的 `NumericLoop` 直接 `limit=2`。策略不能只看是否数值循环，还必须看阶段和动态热度。

### 14.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `sympy_expand` | 1666 | 369 | 143 | 245514 | 1007 | `BranchFSM:193705`, `ReflectionMeta:22772`, `CallDispatcher:18162`, `Mixed:8605` | `LowRoi:236850`, `RiskDefer:7280`, `None:990`, `StartupInit:394` |
| `sympy_integrate` | 2191 | 484 | 256 | 323957 | 1310 | `BranchFSM:253984`, `CallDispatcher:27714`, `ReflectionMeta:27369`, `Mixed:12097` | `LowRoi:309475`, `RiskDefer:12678`, `None:1410`, `StartupInit:394` |
| `sympy_sum` | 3274 | 751 | 519 | 357523 | 1761 | `BranchFSM:261883`, `CallDispatcher:43956`, `ReflectionMeta:30350`, `Mixed:17140` | `LowRoi:343194`, `RiskDefer:11662`, `None:2150`, `StartupInit:517` |
| `sympy_str` | 2026 | 448 | 220 | 412525 | 1135 | `BranchFSM:310889`, `CallDispatcher:44205`, `ReflectionMeta:40657`, `Mixed:14285` | `LowRoi:388041`, `RiskDefer:22912`, `None:1178`, `StartupInit:394` |

### 14.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `sympy_expand` | `sympy.core.mul:Mul.flatten` | 4 | 1806.0ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | expand 核心表达式展平；高成本但属于主体工作，不能全局禁编 |
| `sympy_expand` | `sympy.core.power:Pow._eval_expand_power_base` | 4 | 242.9ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 幂展开核心路径；LowRoi 延迟到高热度合理 |
| `sympy_expand` | `sympy.core.expr:Expr.expand` | 4 | 172.6ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=2`, `risk=HugeCode`, `startup=false` | `2/None` | 主体 API；证明 `ObjectManipulator + codeB=2` 不能全局延迟 |
| `sympy_integrate` | `sympy.integrals.trigonometry:trigintegrate` | 4 | 191.6ms | `NumericLoop`, dims=`Compute+Control`, `loop=2`, `codeB=3`, `risk=HugeCode`, `startup=false`, `compute=true` | `2/None` | 积分核心计算路径，应放行 |
| `sympy_integrate` | `sympy.core.basic:Basic.replace` | 4 | 67.9ms | `Mixed`, dims=`Control+Dynamic`, `loop=2`, `codeB=2`, `risk=Dynamic+Exception+HugeCode`, `startup=false` | `2/None` | 表达式树替换核心路径；即使 high risk，也需看动态收益 |
| `sympy_integrate` | `sympy.printing.printer:Printer._print` | 4 | 54.6ms | `ObjectManipulator`, dims=`Control+Object+Dynamic`, `loop=2`, `codeB=2`, `risk=Exception`, `startup=false` | `2/None` | 输出/打印路径；steady 中不能由 import 策略拦截 |
| `sympy_sum` | `sympy.core.mul:Mul._eval_subs` | 4 | 813.5ms | `BranchFSM`, dims=`Compute+Control`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false`, `compute=true` | `1000/LowRoi` | 求和/替换核心路径；compute=true 但仍低 ROI 延迟，热后编译 |
| `sympy_sum` | `sympy.simplify.simplify:simplify` | 4 | 313.4ms | `ReflectionMeta`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=3`, `risk=HugeCode`, `startup=false` | `2/None` | 符号化简核心路径；反射/动态维度不能全局拦 |
| `sympy_sum` | `sympy.core.expr:Expr.as_terms` | 4 | 107.9ms | `ObjectManipulator`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `2/None` | 表达式项拆分，属于主体工作 |
| `sympy_str` | `sympy.core.mul:Mul.flatten` | 4 | 1763.0ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 字符串化前的表达式处理核心路径 |
| `sympy_str` | `sympy.printing.str:StrPrinter._print_Mul` | 4 | 139.5ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 字符串化核心打印路径；延迟但热后编译 |
| `sympy_str` | `sympy.printing.printer:Printer._print` | 4 | 61.8ms | `ObjectManipulator`, dims=`Control+Object+Dynamic`, `loop=2`, `codeB=2`, `risk=Exception`, `startup=false` | `2/None` | 打印分发核心函数，应按收益判断 |
| `sympy_*` | `sympy.utilities.iterables:least_rotation` | 4 | 46.2-49.2ms | `NumericLoop`, dims=`Compute+Control`, `loop=3`, `codeB=2`, `risk=None`, `startup=true`, `compute=true` | `2/None` | 启动期但有 compute 提示；验证 startup 策略不能拦 compute 函数 |
| `sympy_*` | `sympy.printing.pretty.pretty_symbology:<lambda>` | 50 | 34.2-35.1ms | `Trivial`, dims=`-`, `loop=0`, `codeB=0`, `risk=None`, `startup=true` | `4/LowRoi` | 高频小函数，轻延迟即可，不是 highcost import 目标 |
| `sympy_str/sum` | `mpmath.libmp.libmpf:_normalize` | 4 | 39.2-46.5ms | `BranchFSM`, dims=`Compute+Control`, `loop=1`, `codeB=2`, `risk=None`, `startup=false`, `compute=true` | `1000/LowRoi` | 底层数值规范化；compute=true 但当前受 LowRoi 约束，需非 debug A/B 判断是否应放宽 |

### 14.4 策略判断

| 观察 | 结论 |
|---|---|
| 四个子场景都有大量 `startup=false` 的 `BranchFSM/ObjectManipulator/ReflectionMeta` 大函数 | `sympy` 是 steady highcost 强样本，不能被 import-window 策略粗拦 |
| `Mul.flatten`、`Pow._eval_*`、`StrPrinter._print_Mul` 等 `LowRoi` 函数 gate 数接近 3996 且最终编译 | 1000 阈值在这里是热度过滤，仍保留热后收益 |
| `Expr.expand`、`simplify`、`Basic.replace` 等 highcost 函数是 `2/None` | 全局延迟 `ObjectManipulator/ReflectionMeta/Mixed + codeB>=2` 会直接误伤主体工作 |
| `trigintegrate`、`least_rotation`、`_normalize` 等带 `compute=true` | compute 是收益提示，不能被 startup/import 或 nonnumeric 规则一刀切覆盖 |
| `pretty_symbology:<lambda>` 这类 startup 小函数走 `4/LowRoi` | 小函数轻延迟即可，和 highcost import storm 不是同一类问题 |

## 15 用例：pickle_pure_python 系列

### 15.1 总体判断

`pickle_pure_python` 是纯 Python 序列化 workload，主体不是 C 加速模块，而是 `pickle._Pickler` 内部的大量对象图遍历、分支判断、动态分发和 memo 操作。它和 `dask/sqlalchemy_declarative` 的负 ROI 形态不同：当前正式 A/B 显示，`pickle_pure_python` 的大差距不是 import/setup 编译风暴，也不是 JIT 后 deopt，而是**一开启 CinderX plugin/frame evaluator 就多出来的逐帧税**。

正式口径里，CinderX no-plugin 已经比 CPython JIT 快；优化前 plugin-no-JIT、`PYTHONJITAUTO=2` 和 AutoJIT 都稳定在 `1.06ms` 左右。第一次正式化后，AutoJIT 不再安装 CinderX frame evaluator，并把计数移动到 `jitVectorcall` 的解释返回路径，`pickle_pure_python` 降到 `925us`。继续把 Trivial LowRoi 与长 LowRoi 的等待路径改成分类后解释冻结后，`pickle_pure_python` 降到 `800us`，达到 CPython JIT 85% 目标线；`2to3` 同时从 `1.15s` 降到 `991ms`。

### 15.2 总表：差距账本

口径：`blue-98`，`--warmup 3 --affinity=30`，正式 pyperformance 60 values。CPython JIT 使用 `cpython-baseline:/opt/python314-jit`；CinderX 口径使用 `cinderx-test:/opt/python314`。CinderX plugin/JIT 口径必须通过 `_cinderx_auto` 启动 hook；未加载该 hook 的早期结果不作为结论。

基线：CPython 3.14.3 JIT **684.0us**，85% 目标线约 **804.7us**。优化前 AutoJIT：`auto:2 + provider` **1056.7us**，慢 **+372.7us**。不装 frame evaluator 后 AutoJIT 为 **925us**；LowRoi 冻结正式化后 AutoJIT 为 **800us**，慢 **+116.0us**，已达到 85% 目标线。

| 成本项 | 取数方式 | 优化前差距 | 当前剩余差距 | 已优化掉 | 继续可挖空间 | 下一步判断 |
|---|---|---:|---:|---:|---:|---|
| CinderX 解释器基底 | CinderX no-plugin `643.8us` - CPython JIT `684.0us` | -40.2us | -40.2us | 0.0us | 0.0us | 不是瓶颈；不开插件时 CinderX 路径更快 |
| 旧 plugin/frame evaluator 逐帧税 | 优化前 AutoJIT `1056.7us` - no-plugin `643.8us` | +412.9us | 不再完整存在 | +131.7us | 仍有残留 | auto 模式不再安装 CinderX frame evaluator，但没有消除全部 plugin/gate 成本 |
| LowRoi 等待路径成本 | frame-evaluator 正式化后 `925us` - LowRoi 冻结后 `800us` | +125.0us | 已消除 | +125.0us | 0.0us | Trivial LowRoi 与长 LowRoi 不再执行 classified warmup 等待 |
| 正式 AutoJIT 残留成本 | LowRoi 冻结后 AutoJIT `800us` - no-plugin `643.8us` | 不适用 | +156.2us | 不适用 | ~156us 上界 | 下一步要拆 `jitVectorcall` gate、startup hook、compiled-entry 动态成本 |
| CinderX JIT 动态收益/成本 | `PYTHONJITAUTO=2` - plugin-no-JIT | -0.4us | 未单独复测 | 0.0us | 小 | 优化前 JIT 基本没有改变结果；正式化后需用 gate/compile stats 继续拆 |
| AutoJIT 正式化收益 | LowRoi 冻结后 AutoJIT `800us` - 优化前 AutoJIT `1056.7us` | 0.0us | -256.7us | +256.7us | 已达到 85% 线 | 本次改动有效，但仍可继续挖 plugin/gate 残留 |
| import/setup 编译风暴 | debug 编译事件：import `25`，steady `352` | 非主项 | 非主项 | 0.0us | 不作为主线 | import 编译少，且正式差距在 plugin-no-JIT 已出现 |
| JIT deopt 动态成本 | `PYTHONJITDUMPSTATS=1` | 非主项 | 非主项 | 0.0us | 不作为主线 | 只看到 pyperf harness 的 3 次 `Raise`，没有 pickle 主体 deopt |
| **合计** | 总耗时减 CPython JIT | **+372.7us** | **+116.0us** | **+256.7us** | **已过 85% 线约 4.7us** | 继续优化要拆正式 AutoJIT 残留成本，而不是回到静态分类泛化 |

读表三条结论：

- 本次正式化有效：auto 模式不装 frame evaluator + `jitVectorcall` 解释返回计数 + LowRoi 冻结，合计拿回约 `256.7us`。
- 这个用例已经达到 CPython JIT 85% 线：`800us` 对目标线 `804.7us`，约快 `4.7us`。
- 后续不要回到“大范围禁编 pickle 函数”的方向；应拆正式 AutoJIT 残留的 gate/compiled-entry/startup hook 成本。

### 15.3 分表一：gate 与编译规模

debug 口径：临时 autohook 只 `import _cinderx_auto`，`bm_pickle --pure-python pickle --fast -n 3 -w 1`，只用于函数形状和路径计数，不作为性能数值。5 个 worker/driver 记录合计：

| gate 路径 | 次数 | 含义 |
|---|---:|---|
| `jit_vectorcall` | 49024 | 进入 AutoJIT gate 的总次数 |
| `global_threshold_return` | 6286 | 还没到全局阈值，返回解释执行 |
| `classified_warmup_return` | 39002 | 已分类，但 `LowRoi` 等策略要求继续解释等待更高热度 |
| `classified_defer_freeze` | 3364 | 判定延迟/解释执行，并恢复 interpreted vectorcall |
| `forced_compile` / `forced_compile_ok` | 372 / 372 | 仍然进入 CinderX JIT 编译的次数 |
| `forced_compile_fallback` | 0 | 没有编译失败回退 |

编译事件按阶段和策略分布：

| 维度 | 分布 | 读法 |
|---|---|---|
| 阶段 | `steady=352`，`import=25` | pickle 的 JIT 成本主要在 steady；不是 import 风暴 |
| 策略原因 | `None=258`，`LowRoi=119` | 主要是小对象操作直接放行，少量 pickle helper 延迟到 1000 后仍编译 |
| family | `ObjectManipulator=128`，`BranchFSM=113`，`Trivial=87`，`Mixed=23`，`NumericLoop=18`，`CallDispatcher=8` | 主体是对象图遍历和分支/dispatch，不是数值循环 |
| risk | `risk=0` 为 372 条，`HugeCode=8` 为 5 条 | 当前 debug 口径没有大量 exception-risk 编译 |
| 文件 | `pickle.py=52`，`bm_pickle/run_benchmark.py=10`，其余多为 `argparse/importlib.metadata/contextlib/pyperf` | 只有一部分编译发生在 pickle 主体 |

### 15.4 分表二：代表函数形状

| 函数 | 编译事件 | 阶段 | 形状 | 策略 | 判断 |
|---|---:|---|---|---|---|
| `__main__:bench_pickle` | 5 | steady | `CallDispatcher`, dims=`Dispatch`, `loop=2`, `codeB=2`, `risk=None` | `2/None` | benchmark 本体驱动函数，应放行 |
| `pickle:_Pickler.memoize` | 5 | steady | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None` | `2/None` | memo 小函数，放行合理 |
| `pickle:_Pickler.save_dict` | 5 | steady | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None` | `2/None` | dict 序列化核心小函数 |
| `pickle:_Pickler.save_list` | 5 | steady | `ObjectManipulator`, dims=`Control+Object+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None` | `2/None` | list 序列化核心小函数 |
| `pickle:_getattribute` | 5 | steady | `BranchFSM`, dims=`Control`, `loop=1`, `codeB=0`, `risk=None` | `2/None` | 属性查找分支小函数 |
| `pickle:_Pickler.persistent_id` | 5 | steady | `Trivial`, dims=`-`, `loop=0`, `codeB=0`, `risk=None` | `4/LowRoi` | 极小函数轻延迟；不是主成本 |
| `pickle:_Framer.write` | 5 | steady | `Mixed`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None` | `1000/LowRoi` | framing 写路径，热后编译 |
| `pickle:_Pickler.put` | 5 | steady | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None` | `1000/LowRoi` | memo id 输出路径，热后编译 |
| `pickle:_Pickler.get` | 5 | steady | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None` | `1000/LowRoi` | memo id 读取路径，热后编译 |
| `pickle:_dumps` | 5 | steady | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None` | `1000/LowRoi` | dumps 包装函数，热后编译 |
| `pickle:_Pickler.save_global` | 5 | steady | `BranchFSM`, dims=`Control+Dispatch`, `loop=2`, `codeB=2`, `risk=HugeCode` | `1000/LowRoi` | 全局对象序列化路径；高成本但属于主体工作 |
| `pickle:_Pickler.save_bytes` | 5 | steady | `BranchFSM`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None` | `1000/LowRoi` | bytes 序列化路径，热后编译 |

### 15.5 分表三：deopt 与负面结论

`PYTHONJITDUMPSTATS=1` 的 debug worker 只记录到 3 次 deopt：

| 函数 | 次数 | 原因 | 判断 |
|---|---:|---|---|
| `pyperf._bench:BenchmarkSuite.get_benchmark` | 3 | `Raise` | pyperf harness 正常控制流；不是 pickle 主体 |

负面结论很重要：**不要把 `pickle_pure_python` 归入 dask/sqlalchemy 那类“JIT 后 deopt/guard 动态成本”样本。** 这个用例的劣化在 plugin-no-JIT 已经出现，JIT/AutoJIT 没有明显增量；如果后续做优化，优先级应给 frame evaluator/逐帧计数/gate 早退，而不是针对 pickle 函数继续调分类阈值。

### 15.6 策略判断

| 观察 | 结论 |
|---|---|
| 正式 plugin-no-JIT 已从 `643.8us` 慢到 `1058.9us` | 主差距是 CinderX plugin/frame evaluator 逐帧税 |
| `auto=2` 和 AutoJIT 都约 `1.06ms` | JIT 和 AutoJIT 分类没有解决主差距，也没有新增可观劣化 |
| debug 编译事件 `steady=352`、`import=25` | 不是 startup/import 编译风暴 |
| deopt 只有 pyperf harness 的 3 次 `Raise` | 不是 pickle 主体 deopt/guard 问题 |
| `pickle.py` 主体编译只有 52 条，且多为小对象操作或 LowRoi helper | 继续调 `BranchFSM/codeB` 静态分类不是主杠杆 |
| no-plugin 已快于 CPython JIT | CinderX 解释器基底不是问题；问题发生在装上 plugin 后 |

### 15.7 protocol-core 误伤边界

本轮为修复 `richards` 引入了 steady-state protocol dispatch core 放行条件。中间版本只看“低风险 + 对象/控制/调用”会把 `pickle_pure_python` 的 setup/call-only wrapper 一起放开，导致 `pickle_pure_python` 从 `802us` 退到 `883us-903us`。最终生产条件要求 protocol core 同时满足：

- 小型 steady 低风险实例方法，`code_size_bucket <= 1`，`loop_score = 0`。
- `LOAD_GLOBAL*` 不超过 4 个，调用分发不超过 8 个。
- 有对象状态访问、控制流和返回值。
- 必须有 `STORE_ATTR*` 状态写入，或最多一个 `RAISE_VARARGS` 异常分支。

这条“状态写入或 raise 分支”是关键边界：`richards` 的任务调度核心会改写任务状态或用 assert/raise 表达协议约束；`pickle_pure_python` 里被误放行的多是 call-only wrapper、环境探测和 harness/setup 函数，没有状态更新。最终正式子集里 `pickle_pure_python=800us +- 3us`，与 LowRoi 冻结后的最好结果持平。

## 16 用例：deepcopy 系列

### 16.1 总体判断

`deepcopy` 一次 benchmark run 覆盖三个子场景：标准对象深拷贝、`__reduce__` 路径和 memo 复用路径。真实 worker gate 证据显示，核心收益路径集中在 `copy._reconstruct`、`copy._deepcopy_dict/list/tuple` 和三个 `__main__:benchmark*` 驱动函数。它们都是 steady 对象图遍历和重建，不属于 startup/import。

这组样本进一步说明：对象图 workload 不能按“非数值 + 控制/对象 + highcost”全局粗拦。`copy._reconstruct` 被 `LowRoi` 延迟到 1000，但 gate 数接近 7992 且最终编译；`copy.deepcopy` 入口本身在本口径下没有编译，说明当前策略会把热点放到更具体的内部函数，而不是盲目编译所有入口。

2026-06-08 的 deopt 穿刺补充了一个更细的结论：`deepcopy` 系列确实是典型 JIT 用例，但 CinderX 当前对“用 `try/except KeyError` 表示正常 miss 路径”的处理有动态成本。`copy._deepcopy_tuple` 和 `copy._keep_alive` 都会在 JIT 中产生 `UnhandledException/DictSubscr` deopt；但二者的策略含义不同。只抑制 `_deepcopy_tuple` 在正式 pyperformance 中有小幅正收益；尝试放宽 `_keep_alive` 虽然让 `deepcopy_memo` 有非显著小幅改善，但会让 `deepcopy_reduce` 明显回归。因此，最终生产策略只新增 looped expected-exception 形状延迟，`_keep_alive` 保持既有 `RiskDefer`。

### 16.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `deepcopy` | 1647 | 193 | 14 | 126328 | 529 | `BranchFSM:96558`, `CallDispatcher:10238`, `ReflectionMeta:8966`, `Mixed:8160` | `LowRoi:120574`, `RiskDefer:4330`, `None:1086`, `StartupInit:338` |

### 16.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `deepcopy` | `copy:_reconstruct` | 8 | 190.1ms | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `1000/LowRoi` | 深拷贝对象重建核心路径；延迟到高热度后编译 |
| `deepcopy` | `__main__:benchmark` | 4 | 48.2ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | 标准对象深拷贝本体，应放行 |
| `deepcopy` | `copy:_deepcopy_dict` | 12 | 31.2ms | `CallDispatcher`, dims=`Control+Object+Dispatch`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | dict 深拷贝核心函数，放行合理 |
| `deepcopy` | `__main__:benchmark_reduce` | 4 | 14.1ms | `CallDispatcher`, dims=`Object+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | `__reduce__` 子场景驱动函数，应放行 |
| `deepcopy` | `copy:_deepcopy_list` | 8 | 13.6ms | `CallDispatcher`, dims=`Control+Object+Dispatch`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | list 深拷贝核心函数，放行合理 |
| `deepcopy` | `copy:_deepcopy_tuple` | 8 | 13.2ms | `BranchFSM`, dims=`Control`, `loop=2`, `codeB=1`, `risk=Exception`, `startup=false` | `1000/LowRoi` | tuple memo miss 用 expected `KeyError` 表达；正式穿刺显示只抑制该函数有小幅正收益，是 expected-exception 收窄候选 |
| `deepcopy` | `__main__:benchmark_memo` | 4 | 12.6ms | `ObjectManipulator`, dims=`Object+Dispatch`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | memo 子场景驱动函数，应放行 |
| `deepcopy` | `copy:_reconstruct.<locals>.<genexpr>` | 4 | 7.0ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `suspend=true`, `startup=false` | `2/None` | suspendable 小热路径，不能因为 suspendable 一刀切禁编 |
| `deepcopy` | `copyreg:__newobj__` | 4 | 4.4ms | `Mixed`, dims=`Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `1000/LowRoi` | reduce 重建辅助函数，低 ROI 延迟后热度足够时编译 |
| `deepcopy` | `copy:deepcopy` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Dispatch`, `loop=0`, `codeB=2`, `risk=None`, `startup=false` | `2097152/LowRoi` | 入口函数未编译，当前策略更偏向内部热点 |
| `deepcopy` | `copy:_keep_alive` | 0 | 0.0ms | `BranchFSM`, dims=`Control`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=false` | `2097152/RiskDefer` | 同样有 expected `KeyError` deopt；生产 A/B 证明放宽它会让 `deepcopy_reduce` 明显回归，最终保持 `RiskDefer` |
| `deepcopy` | `dataclasses:_process_class` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=true` | `2097152/RiskDefer` | dataclass 初始化噪声，import/setup 风险延迟正确 |

### 16.4 策略判断

| 观察 | 结论 |
|---|---|
| `copy._reconstruct` 是 `startup=false + LowRoi` 且最终编译 | deep copy 的对象重建核心路径应允许热后编译 |
| `copy._deepcopy_dict/list` 和三个 `__main__:benchmark*` 驱动函数均为 `2/None` | 小而热的对象图操作应直接放行 |
| `copy.deepcopy` 入口未编译，内部热点编译 | 当前策略不会盲目编译所有入口函数，这对对象图 workload 是有利边界 |
| `copy._reconstruct.<locals>.<genexpr>` 是 suspendable 但放行 | suspendable 只能作为风险信号，不能单独决定禁编 |
| dataclass startup 样本走 `RiskDefer/StartupInit` | import/setup 保护有效，但不应用来否定 steady deepcopy 热路径 |
| `_deepcopy_tuple` 的 deopt 来自 memo miss 的 expected `KeyError` | 这是 AutoJIT 可优化对象；只抑制该函数相对原始 `PYTHONJITAUTO=2` 几何均值约 1.02x faster |
| `_keep_alive` 的 deopt 数更高，但 loop-free 小 helper 与 `_deepcopy_tuple` 的 looped miss 形状不同 | deopt 数不是策略条件；最终保持既有 `RiskDefer`，避免 `deepcopy_reduce` 回归 |

### 16.5 deopt 与正式 A/B 穿刺

以下 deopt 数来自固定迭代数 probe，用来定位动态成本来源，不直接当作正式耗时。`DictSubscr` 对应 `copy.py` 中在 `try/except KeyError` 内访问 memo 字典，KeyError 是正常控制流，不是业务异常。

| 口径 | `deepcopy` deopt | `deepcopy_memo` deopt | `deepcopy_reduce` deopt | 结论 |
|---|---:|---:|---:|---|
| 原始 CinderX：`PYTHONJITAUTO=2` | 360000：`_keep_alive` 240000，`_deepcopy_tuple` 120000 | 40000：二者各 20000 | 60000：`_keep_alive` | 验证 try/except miss 路径确实造成大量 JIT deopt |
| 当前 AutoJIT：`auto:2 + find_and_load` | 120000：`_deepcopy_tuple` | 20000：`_deepcopy_tuple` | 0 | 当前策略已经避开 `_keep_alive`，但仍放行 `_deepcopy_tuple` |
| 原始 CinderX + 只抑制 `_deepcopy_tuple` | 240000：`_keep_alive` | 20000：`_keep_alive` | 60000：`_keep_alive` | `_deepcopy_tuple` deopt 可单独清除，且不影响 `_keep_alive` 的 JIT 收益 |

正式 pyperformance 口径为 `--warmup 3 --affinity=30 -b deepcopy`，同一 candidate wheel、同一 venv，对比 `PYTHONJITAUTO=2`、当前 AutoJIT 和只抑制 `_deepcopy_tuple` 的原型：

| 对照 | `deepcopy` | `deepcopy_memo` | `deepcopy_reduce` | 几何均值 |
|---|---:|---:|---:|---:|
| `PYTHONJITAUTO=2` -> 当前 AutoJIT | 891us -> 904us，1.01x slower | 85.7us -> 96.0us，1.12x slower | not significant | 1.04x slower |
| 当前 AutoJIT -> 当前 AutoJIT + 抑制 `_deepcopy_tuple` | 904us -> 871us，1.04x faster | 96.0us -> 95.0us，1.01x faster | not significant | 1.02x faster |
| `PYTHONJITAUTO=2` -> `PYTHONJITAUTO=2` + 抑制 `_deepcopy_tuple` | 891us -> 875us，1.02x faster | 85.7us -> 83.5us，1.03x faster | 9.25us -> 9.13us，1.01x faster | 1.02x faster |

策略结论：`deepcopy` 有 AutoJIT 优化潜力，但幅度是小到中等，主要来自避免 `_deepcopy_tuple` 的 expected-exception deopt。不要把该结论泛化成“所有异常风险函数延迟”：`_keep_alive` 反例说明，异常边成本可能被函数本身的动态收益覆盖。

生产实现复测进一步收敛了策略边界：只新增 `BranchFSM + loop_score>=2 + code_size_bucket=1 + risk=Exception + active_dim=Control` 的 steady-state 延迟；不放宽 `loop_score=0 + code_size_bucket=0 + risk=Exception` 的小 helper。`blue-98:/results/autojit-exception-policy-20260608` 中，tuple-only 策略相对旧 wheel 的小子集结果为：`2to3` 1.37x faster，`deepcopy` 1.04x faster，`deepcopy_memo` 和 `deepcopy_reduce` 均 not significant；错误放宽 `_keep_alive` 的对照中，tuple-only 对 `deepcopy_reduce` 反而 1.10x faster，证明 `_keep_alive` 不应放行。`nbody` 在本轮出现 4%-6% slower 且方差较大，同一 wheel 的 `PYTHONJITAUTO=2` 为 111ms，`auto:2` 为 115-118ms，标记为独立待查，不归因于 `_deepcopy_tuple` 形状规则。

## 17 用例：dask

### 17.1 总体判断

`dask` 是 steady-state 异步调度、消息序列化、状态机更新和事件循环 workload，不是 startup/import compile storm。`CinderX no-plugin` 本身不慢，`plugin-no-JIT` 也基本追平 CPython JIT；真正把用例拉慢的是启用 CinderX JIT 后的动态成本。v0.23 账本里，`auto=2` 到当时 AutoJIT 的均值差只有 `+16.3ms`，小于本用例 `~106ms` 标准差。

结论先写在前面：`dask` 不应驱动 import/setup 策略继续泛化。它是 JIT 动态 ROI 负样本，优化方向应是 deopt/guard/expected-exception 专项，或更细的 steady-state 动态反馈，而不是把 startup provider 或静态 highcost 规则继续放大。

2026-06-10 的新结论是另一个层面：`generators` 修复引入的 plain generator steady-state override 不能覆盖 `@types.coroutine` 生成的 generator-based coroutine。`asyncio.tasks:__sleep0` 就是这种函数，它服务于 `asyncio.sleep(0)`；一旦被 JIT 编译，会破坏 awaitable 语义并让 dask 直接失败。修复后 `dask` 可以跑完，但正式结果仍是 `1.77s +- 0.05s`，比 CPython JIT/plugin-no-JIT 慢 `1.30x`，所以性能主问题仍未根治。

### 17.2 总表：差距账本

注意：本小节保留 v0.23 初始账本，用来说明 dask 的原始差距来源；它不是 v0.33 的最新主线性能。RoiBackoff、per-site deopt 和 noattr 复核见 §17.2.1 与 §17.4.1。

口径：`blue-98`，`--warmup 3 --affinity=30`，正式 pyperformance 60 values。CPython JIT 使用 `/opt/python314-jit/bin/python3.14`；CinderX 口径使用同一 `/opt/python314` 和同一 dask venv，只改变 `CINDERX_PLUGIN_ENABLE` / `PYTHONJITAUTO` / provider 环境变量。

基线：CPython 3.14.3 JIT **1360.2ms**。优化前：CinderX `PYTHONJITAUTO=2` **2005.5ms**，慢 **+645.2ms**。当前 AutoJIT：`auto:2 + provider` **2021.8ms**，慢 **+661.5ms**。

下表把这 `+661.5ms` 按成本来源平铺。`已优化掉` = 优化前差距 - 当前剩余差距；负数表示当前 AutoJIT 在该项没有收益，差值在本用例方差内但不能算正收益。

| 成本项 | 取数方式 | 优化前差距 | 当前剩余差距 | 已优化掉 | 继续可挖空间 | 下一步判断 |
|---|---|---:|---:|---:|---:|---|
| CinderX 解释器基底 | CinderX no-plugin `1289.2ms` - CPython JIT `1360.2ms` | -71.0ms | -71.0ms | 0.0ms | 0.0ms | 不是瓶颈；不开插件时 CinderX 路径反而略快 |
| 插件固定加载/初始化 | CinderX plugin-no-JIT `1355.4ms` - CinderX no-plugin `1289.2ms` | +66.2ms | +66.2ms | 0.0ms | 小 | 有固定成本，但被解释器基底的 -71.0ms 抵消后，plugin-no-JIT 已基本追平 CPython JIT |
| CinderX JIT 动态成本 | `PYTHONJITAUTO=2` / 当前 AutoJIT 分别减 plugin-no-JIT | +650.1ms | +666.4ms | -16.3ms | ~666ms 上界 | **主差距**；来自 steady 阶段编译后运行期 deopt/guard/expected exception，不是 startup/import |
| import/startup 编译风暴 | debug 编译事件：import `45`，steady `3440` | 非主项 | 非主项 | 0.0ms | 不作为主线 | import 只占 `45/3485` 个编译事件，不应继续泛化 startup/import 规则 |
| **合计** | 总耗时减 CPython JIT | **+645.2ms** | **+661.5ms** | **-16.3ms** | **~666ms 上界** | 当前 AutoJIT 没解决 dask；下一步只能看 JIT 动态成本专项，或做 deopt-aware 禁编策略 |

读表三条结论：

- dask 的基底不慢：CinderX no-plugin 比 CPython JIT 快 `71.0ms`；即使加上插件固定成本，plugin-no-JIT 也只比 CPython JIT 快 `4.8ms`。
- 真正差距只有一项：启用 CinderX JIT 后新增 `+650ms` 级动态成本；当前 AutoJIT 没把它降下来。
- 下一步不该继续调 startup/import/provider，而应围绕 `distributed`/`asyncio`/`cloudpickle`/`zict` 的 deopt、slot guard 多态、expected exception 慢路径做专项 A/B。

### 17.2.1 2026-06-11 复核

本轮按“先确认 deopt 风暴是否还在，再复核小收益开关”的顺序执行。正式性能复核使用串行同核口径：两轮都在 `blue-98:cinderx-test`，同一 worker venv `/root/venv/cpython3.14-43f131f998a6-compat-31b33d68c68a`，同一 `--affinity=30 --warmup 3 -b dask`，唯一差异是第二轮额外设置 `PYTHONJITATTRCACHES=0`。CPU 0-47 可用，CPU 30 对应 core 30；本轮没有并行抢同一核。

环境契约：

| 项 | 证据 |
|---|---|
| worker 命令 | `/root/venv/cpython3.14-43f131f998a6-compat-31b33d68c68a/bin/python -u .../bm_dask/run_benchmark.py --affinity=30 --warmups=3` |
| worker venv | `include-system-site-packages = true`，system site 包含 `/opt/python314/lib/python3.14/site-packages/cinderx.pth` 与 `__editable__.cinderx-2026.6.9.0.pth` |
| JIT 初始化 | worker 内 `cinderx.__file__=/cinderx/cinderx/PythonLib/cinderx/__init__.py`，`_cinderx.__file__=/cinderx/cinderx/PythonLib/_cinderx.so`，`cinderx.get_import_error()=None`，`cinderx.is_initialized()=True` |
| 继承变量 | `CINDERX_PLUGIN_ENABLE`、`PYTHONJITAUTO`、`PYTHONJITHUGEPAGES`、`PYTHONJITATTRCACHES`、`LD_LIBRARY_PATH`、`PYTHONPATH`、代理变量 |
| 正式/诊断 | 正式性能数据清理了 `PYTHONJITLOGFILE`、`PYTHONJITDUMPSTATS`、HIR/LIR dump、gate stats、compile events 等诊断变量 |

复核结果：

| 实验 | 产物 | 结果 | 结论 |
|---|---|---:|---|
| RoiBackoff on/off 历史正式 A/B | `/results/autojit-dask-roi-ab-20260611_175540` | off `1.67s +- 0.05s`，on `1.62s +- 0.04s` | RoiBackoff 对 dask 有约 `1.03x` 小收益，但不是根治 |
| 当前默认 AutoJIT 串行同核 | `/results/autojit-dask-noattr-serial-aff30-20260611_212203/default.json` | `1.68s +- 0.05s`，60 values，无 outlier，pyperf 提示样本不足以证明 `<1%` 稳定性 | 当前主线仍在 `1.6s+`，离 CPython JIT `1.36s` 仍有约 `+320ms` |
| `PYTHONJITATTRCACHES=0` 串行同核 | `/results/autojit-dask-noattr-serial-aff30-20260611_212203/noattr.json` | `1.61s +- 0.04s`，相对默认 `1.04x faster` | noattr 正信号在同核串行口径下复现，但它是全局 JIT 开关，不能直接默认化 |
| 关闭 LOAD_ATTR_SLOT mismatch fallback 穿刺 | `/results/autojit-dask-loadattr-fallbackoff-formal-20260611_204911` | `1.66s +- 0.06s`，且 site 分布仍为 `129` deopt | 关闭 fallback 没有稳定收益；LOAD_ATTR_SLOT 不是当前 dask 的主杠杆 |
| 关闭 array double fastpath 穿刺 | `/results/autojit-dask-noarray-formal-20260611_210653` | `1.68s +- 0.05s` | 负收益，历史 worker 单值信号未复现 |
| noattr + noarray 穿刺 | `/results/autojit-dask-noattr-noarray-formal-20260611_210653` | `1.65s +- 0.05s` | noarray 抵消 noattr 收益，不保留 |

读表结论：

| 问题 | 当前答案 |
|---|---|
| RoiBackoff 是否有效 | 有效。它把一类 expected-exception/调用类风暴压下去，正式性能约 `1.03x` 小收益。 |
| 当前 dask 还慢在哪里 | 仍慢在启用 CinderX JIT 后的 steady 动态成本；不是 startup/import/provider。 |
| LOAD/STORE slot 全局 fallback 是否是方向 | 不是。LOAD fallback-off 不改善；历史 STORE fallback 也无收益。全局改 lowering 会影响大量单态站点的精化收益。 |
| noattr 是否值得做 | 值得继续研究，但只能做“局部策略/PIC/站点级泛化”方向；不能把 `PYTHONJITATTRCACHES=0` 作为生产默认。 |
| noarray 是否值得做 | 不值得。正式口径负收益。 |

### 17.2.2 RoiBackoff 默认开启守门批次

本批次回答一个发布问题：`CINDERX_AUTOJIT_ROI_BACKOFF=1` 能否从实验开关变成默认行为。共同口径：`blue-98:cinderx-test`，CinderX wheel 重新安装为 `2026.6.11.0`，worker venv `include-system-site-packages=true`，worker 内 `_cinderx_auto_loaded=True`，无 `PYTHONPATH` 污染；pyperformance 使用 `--warmup 3`。off/on 只切换 `CINDERX_AUTOJIT_ROI_BACKOFF=0/1`，其它核心变量固定为 `CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2`。

产物：`/results/autojit-roi-backoff-guard-20260611_221949`。

| 用例 | off | on | on/off | 结论 |
|---|---:|---:|---:|---|
| `2to3` | `570.765ms` | `570.079ms` | `0.999x` | 无误伤；对主用例持平 |
| `deepcopy` | `687.708us` | `683.638us` | `0.994x` | 无误伤 |
| `deepcopy_reduce` | `6.952us` | `6.941us` | `0.998x` | 历史 `_keep_alive` 误伤样本持平 |
| `deepcopy_memo` | `72.019us` | `71.641us` | `0.995x` | 无误伤 |
| `generators` | `18.587ms` | `18.533ms` | `0.997x` | 历史 `Tree.__iter__` 误伤样本未回归 |
| `pickle_pure_python` | `803.304us` | `800.289us` | `0.996x` | 无误伤 |
| `sqlalchemy_declarative` | `310.465ms` | `310.450ms` | `1.000x` | 负 ROI 样本持平；本批次未看到额外收益或误伤 |
| `nbody` 初始并行 | `111.548ms` | `119.021ms` | `1.067x` | 并行绑不同 CPU 区间，疑似 NUMA/频率噪声；不作为误伤结论 |
| `richards` 初始并行 | `102.216ms` | `103.594ms` | `1.014x` | 初始轻微劣化需同核复核 |
| `nbody` 同核串行复跑 | `119.128ms` | `111.655ms` | `0.937x` | 初始劣化不复现，on 更快 |
| `richards` 同核串行复跑 | `103.775ms` | `102.424ms` | `0.987x` | 初始劣化不复现，on 持平略快 |

配套 smoke：

| 项 | 结果 | 结论 |
|---|---|---|
| RuntimeTest `BehaviorClassifierRuntimeTest.RoiBackoffUncompilesDeoptStorm` | 通过 | deopt 出口触发 uncompile/退避的基本路径可用 |
| gdb batch smoke | 容器内 `ptrace: Operation not permitted`，`Seccomp: 2`；`docker exec --privileged` 仍失败；宿主无 gdb/目标 Python | 当前环境不能证明 gdb smoke 通过，也不能证明功能失败；作为允许 ptrace 环境下的补验项 |

发布判断：

| 问题 | 结论 |
|---|---|
| 是否有明确收益 | 有。dask on/off 已证明 deopt 从 `64164` 降到 `129`，正式性能 `1.67s -> 1.62s`，RoiBackoff 对 steady deopt 风暴有止血作用 |
| 是否误伤守门样本 | 本批次未发现。`deepcopy_reduce`、`generators`、`richards`、`nbody` 经复核不回归 |
| 是否可以默认开启 | 可以，前提是保留 `CINDERX_AUTOJIT_ROI_BACKOFF=0` 为止血退路，并把 budget/rounds/rewarm/mask 写入 `autojit_config_id` |
| 还缺什么 | 允许 ptrace 的环境补跑 gdb smoke；后续修改 reason mask / budget / rounds 需重新跑同类守门 |

### 17.2.3 合并 upstream 后复核

合并 upstream 后带入 descriptor inline cache、协程运行时路径和 `_cinderx.so` 内部 PLT 优化。由于这些改动正好覆盖 dask 剩余差距中的 attr-cache、async/coroutine 和固定调用开销，v0.33 的 noattr/per-site 证据不能继续跨配置引用。本轮先同步本地 tracked 源码到 `blue-98:cinderx-test`，重装 editable CinderX，再跑 L1/L3 和 dask 复核。

功能测试：

| 范围 | 结果 |
|---|---|
| `test_autojit_gate_stats.py` | `14 passed` |
| RuntimeTests：`BehaviorClassifierTest.*:BehaviorClassifierRuntimeTest.*:InlineCacheTest.*:JITGeneratorTest.*` | `57 passed` |

正式性能口径：`CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 PYTHONJITHUGEPAGES=0`，`--warmup 3 --affinity=30`，worker venv `/cinderx/venv/cpython3.14-43f131f998a6-compat-31b33d68c68a`，`include-system-site-packages=true`，`pip freeze` 含 `-e /cinderx`。正式数据不带 dump/HIR/JIT log 变量。

| 口径 | 产物 | 结果 | 结论 |
|---|---|---:|---|
| default AutoJIT | `/results/autojit-dask-after-upstream-20260612_003254/default.json` | `1.58s +- 0.06s` | 合并 upstream 后默认 dask 比 v0.33 的 `1.68s` 改善约 `100ms` |
| `PYTHONJITATTRCACHES=0` | `/results/autojit-dask-after-upstream-20260612_003254/noattr.json` | `1.58s +- 0.05s` | `pyperf compare_to` 不显著；v0.33 的 `1.04x` noattr 小收益在 !103 后不复现 |

per-site deopt 诊断口径：`--fast --warmup 1`，`PYTHONJITDUMPSTATS=1`，`PYTHONJITLOGFILE=$RESULTS/jit-{pid}.log`。合并后 `PYTHONJITLOGFILE` 只识别 `{pid}`，不能再用旧 `%p` 模板；使用 `%p` 会把多个 worker 写进同一个 `jit-%p.log` 并导致并发日志交错，该口径作废。

| 口径 | 总 deopt | reason 分布 | top site | 结论 |
|---|---:|---|---|---|
| RoiBackoff off | `61912` | `UnhandledException=61694`、`GuardFailure=131`、`Raise=87` | `set_thread_state VectorCall=30792`、`LRU.__delitem__ DeleteSubscr=30792` | expected-exception/调用类风暴仍在 |
| RoiBackoff on | `218` | `GuardFailure=131`、`UnhandledException=55`、`Raise=32` | `SpansSchedulerExtension.heartbeat GuardType=48`、`StateMachineEvent.to_loggable STORE_ATTR_SLOT=28`、`Scheduler.heartbeat_worker GuardType=24` | RoiBackoff 止血仍成立，大风暴被压平 |

更新后的判断：

| 问题 | 合并后答案 |
|---|---|
| noattr 还值得作为当前优化方向吗 | 暂不作为主线。descriptor inline cache 合入后，`PYTHONJITATTRCACHES=0` 与 default 持平，旧的 `1.04x` 小收益失效 |
| RoiBackoff 还有效吗 | 有效。off/on deopt 从 `61912` 降到 `218`，仍解释了为什么默认开启有价值 |
| dask 剩余差距变了吗 | 变小了。相对旧 CPython JIT 基线 `1.360s`，当前 default 约 `1.16x slower`，剩余差距约 `+220ms`；若要冻结最新对标，应再跑一轮 CPython JIT baseline |
| 下一步 | 重新铺平合并后的 dask 阶段账本；不要继续引用 v0.33 的 noattr 结论作为优化依据 |

### 17.3 分表一：gate 与编译规模

debug 口径：`--fast -n 3 -w 1`，只用于函数形状和路径计数，不作为性能数值。4 个 pyperf worker 的合计数据如下。

| gate 路径 | 次数 | 含义 |
|---|---:|---|
| `jit_vectorcall` | 962700 | 进入 AutoJIT gate 的总次数 |
| `global_threshold_return` | 50864 | 还没到全局阈值，返回解释执行 |
| `classified_warmup_return` | 747293 | 已分类，但继续解释等待更高热度 |
| `classified_defer_freeze` | 161135 | 判定延迟/解释执行，并恢复 interpreted vectorcall |
| `forced_compile` / `forced_compile_ok` | 3408 / 3404 | 仍然进入 CinderX JIT 编译的次数 |
| `forced_compile_fallback` | 4 | 编译失败/回退 |

编译事件按阶段和策略分布：

| 维度 | 分布 | 读法 |
|---|---|---|
| 阶段 | `steady=3440`，`import=45` | dask 的 JIT 成本几乎都发生在 steady 阶段 |
| 策略原因 | `None=2027`，`LowRoi=1458`，`StartupInit=0`，`RiskDefer=0` | import/setup 风暴规则基本不是主路径；`LowRoi` 仍会在热度足够后编译 |
| family | `BranchFSM=1273`，`ObjectManipulator=1266`，`Trivial=577`，`CallDispatcher=128`，`Mixed=92`，`NumericLoop=73`，`ReflectionMeta=68`，`AsyncStateMachine=4` | 主体是控制/对象/调度形状，不是数值循环 |
| unique compiled | 812 | 编译面很宽，属于框架型异步 workload |

代表性编译函数：

| 函数 | 编译事件 | 阶段 | 形状 | 策略 | 判断 |
|---|---:|---|---|---|---|
| `asyncio.futures:_chain_future.<locals>._set_state` | 77 | steady | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=-` | `1000/LowRoi` | 热到越过 LowRoi 阈值；继续编译 |
| `asyncio.futures:_chain_future.<locals>._call_check_cancel` | 77 | steady | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=-` | `1000/LowRoi` | 同上，异步 future 状态传播热点 |
| `distributed.metrics:ContextMeter.meter.<locals>.callback` | 43 | steady | `BranchFSM`, dims=`Control+Dynamic`, `loop=0`, `codeB=0`, `risk=-` | `1000/LowRoi` | 高频回调；静态看低 ROI，但热度足够 |
| `distributed.worker:Worker.execute` | 12 | steady | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode` | `1000/LowRoi` | dask worker 核心执行路径；高成本但不是 startup |
| `distributed.scheduler:Scheduler.add_worker` | 8 | steady | `BranchFSM`, dims=`Control+Dispatch`, `loop=2`, `codeB=1`, `risk=Exception` | `1000/LowRoi` | scheduler 生命周期路径；热后仍编译 |
| `distributed.scheduler:Scheduler.remove_worker` | 8 | steady | `BranchFSM`, dims=`Control+Dispatch`, `loop=2`, `codeB=1`, `risk=Exception` | `1000/LowRoi` | 同上 |
| `distributed.worker:Worker.handle_scheduler` | 8 | steady | `Mixed`, dims=`Control`, `loop=3`, `codeB=1`, `risk=Suspend+Exception` | `2/None` | suspend/exception 状态机，当前直接放行 |
| `distributed.client:Client.map` | 4 | steady | `ReflectionMeta`, dims=`Control+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode` | `2/None` | benchmark 主体映射入口，不能用 import 规则拦 |
| `dask.highlevelgraph:HighLevelGraph.from_collections` | 4 | steady | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=-` | `2/None` | dask 图构建核心路径 |
| `distributed.comm.core:connect` | 4 | steady | `ObjectManipulator`, dims=`Control+Object+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode` | `2/None` | 通信连接路径，高成本但属主体工作 |

### 17.4 分表二：JIT 动态成本

`PYTHONJITDUMPSTATS=1` 的 debug worker 统计显示，dask 的主要问题不是“编译了几个 import 函数”，而是 JIT 后运行期反复 deopt。4 个 worker 合计：

| deopt 类别 | 次数 | 占比 | 读法 |
|---|---:|---:|---|
| `GuardFailure` | 747087 | 73.2% | 类型/slot 形态多态，JIT guard 经常失效 |
| `UnhandledException` | 245480 | 24.0% | 字典/删除/调用等 expected exception 或慢路径被 JIT 当异常成本处理 |
| `Raise` | 28187 | 2.8% | 业务中正常 raise/control path |
| **合计** | **1020754** | **100%** | 动态成本规模远大于 import 编译小账 |

top deopt site：

| 函数 | 次数 | 原因 | 说明 |
|---|---:|---|---|
| `distributed.scheduler:TaskCollection.transition` | 152512 | `GuardFailure/LOAD_ATTR_SLOT` | scheduler 状态对象 slot guard 多态 |
| `distributed.worker_state_machine:StateMachineEvent.__new__` | 92490 | `GuardFailure/STORE_ATTR_SLOT` | worker 状态机事件对象构造多态 |
| `distributed.worker_state_machine:WorkerState.handle_stimulus` | 61690 | `GuardFailure/LOAD_ATTR_SLOT` | 多种 stimulus/event 类型进入同一路径 |
| `functools:_singledispatchmethod_get.__call__` | 61690 | `GuardFailure/LOAD_ATTR_SLOT` | singledispatch 绑定目标多态 |
| `distributed.core:Server.digest_metric` | 56731 | `GuardFailure/GuardType` | metric value/type 不稳定 |
| `asyncio.events:Handle.__init__` | 44193 | `GuardFailure/STORE_ATTR_SLOT` | asyncio handle 类型变化 |
| `cloudpickle:_function_getstate` | 31716 | `GuardFailure/LOAD_ATTR_SLOT` | 函数序列化属性访问多态 |
| `cloudpickle:Pickler._function_getnewargs` | 31716 | `GuardFailure/LOAD_ATTR_SLOT` | 同上 |
| `zict.cache/lru/func:__delitem__` | 92376 | `UnhandledException/DeleteSubscr` | cache/lru 删除路径 expected miss/慢路径 |
| `distributed.worker_state_machine:WorkerState._handle_compute_task` | 30792 | `UnhandledException/DictSubscr` | 状态字典访问 miss 路径 |
| `distributed.protocol.pickle:dumps` | 29087 | `UnhandledException/CallEx` + `GuardFailure/GuardType` | 序列化调用形态复杂 |
| `distributed.client:Future._verify_initialized` | 28000 | `Raise/Raise` | Future 状态校验中的正常控制流 |

### 17.4.1 当前 RoiBackoff 后的 deopt 复核

上表是 v0.23 历史账本，用来说明 dask 为什么是 RoiBackoff 的动机样本。当前版本启用 RoiBackoff 后，百万级 deopt storm 已经不再是当前状态。2026-06-11 用新增的 per-site 字段重新 dump：

| 口径 | 产物 | 总 deopt | top 分布 | 判断 |
|---|---|---:|---|---|
| RoiBackoff off | `/results/autojit-dask-site-fields-roioff-20260611_203926` | `64164` | `zict.lru:LRU.__delitem__` `DeleteSubscr=30792`；`distributed.utils:set_thread_state` `VectorCall=30792`；`asyncio.base_events:BaseEventLoop.call_later` `GuardType=2367` | expected-exception/调用类风暴仍会出现；RoiBackoff 的动机成立 |
| RoiBackoff on | `/results/autojit-dask-site-fields-roion-20260611_203926` | `129` | `distributed.spans:SpansSchedulerExtension.heartbeat` `GuardType=52`；`distributed.worker_state_machine:StateMachineEvent.to_loggable` `STORE_ATTR_SLOT=28`；`distributed.scheduler:Scheduler.heartbeat_worker` `GuardType=24`；`Scheduler.remove_worker` `CallMethod=20` | 大风暴已被压到百级，剩余 deopt 不是 LOAD_ATTR_SLOT 主导 |

per-site 字段让每个 deopt 事件能落到 `bc_offset/deopt_idx/opcode/specialized_opcode`：

| 代表 site | `bc_offset` | `deopt_idx` | `opcode/specialized_opcode` | 次数 | 读法 |
|---|---:|---:|---:|---:|---|
| `distributed.spans:SpansSchedulerExtension.heartbeat` | `182` | `23/24` | `86/86` | `28/24` | 当前 RoiBackoff on 后的最大 GuardType 小项 |
| `distributed.worker_state_machine:StateMachineEvent.to_loggable` | `2` | `1` | `87/87` | `28` | STORE_ATTR_SLOT 小项，不支持全局 STORE fallback |
| `distributed.scheduler:Scheduler.heartbeat_worker` | `404` | `38` | `44/133` | `24` | scheduler 心跳小项 |
| `zict.lru:LRU.__delitem__`（RoiBackoff off） | `88` | `6` | `8/8` | `30792` | 关闭 RoiBackoff 后 expected `DeleteSubscr` 风暴立即出现 |
| `distributed.utils:set_thread_state`（RoiBackoff off） | `44` | `8` | `52/52` | `30792` | 关闭 RoiBackoff 后调用类风暴立即出现 |

这组复核改变了 dask 的下一步优先级：

| 原假设 | 复核后处理 |
|---|---|
| LOAD_ATTR_SLOT 风暴仍是当前主项 | 否。当前 RoiBackoff on 后只有 `129` 次 deopt，LOAD_ATTR_SLOT 不再是 top 项。 |
| 全局 slot fallback 能解决 dask | 否。LOAD fallback-off 正式 `1.66s +- 0.06s` 无收益；历史 STORE fallback 也无收益。 |
| 禁编 top GuardFailure 函数能解决 dask | 不足。RoiBackoff 已经能压大风暴，但默认仍在 `1.6s+`，剩余差距还包含 attr cache、call/frame 固定成本、解释/JIT 切换等小账。 |
| 继续调 AutoJIT 静态分类能解决 dask | 不足。当前最清晰的小正信号来自 `PYTHONJITATTRCACHES=0`，这是 JIT codegen/runtime 行为，不是 startup/import 分类。 |

### 17.5 2026-06-10 直接失败链路

| 阶段 | 证据 | 判断 |
|---|---|---|
| 失败现象 | `dask --fast` 失败，worker/scheduler 断连，日志中 `asyncio.sleep(0)` -> `__sleep0()` 抛 `TypeError: 'generator' object can't be awaited` | 这是功能性回归，不是单纯性能劣化 |
| 触发函数 | `asyncio.tasks:__sleep0`，`co_flags=0x4000123`，同时有 `CO_GENERATOR` 和 `CO_ITERABLE_COROUTINE` | 它不是普通 generator，而是 generator-based coroutine |
| 错误准入 | 失败 compile-events：`fullname=asyncio.tasks:__sleep0`，`calls=2`，`effective_limit=2`，`branch_reason=None`，`family=BranchFSM`，`is_suspendable=true` | plain generator override 只看 `CO_GENERATOR`，把 `CO_ITERABLE_COROUTINE` 误放行 |
| 运行后果 | JIT 编译后返回的 generator 不能被 `await` 接受，dask worker 执行中的 `asyncio.sleep(0)` 失败 | scheduler 连接丢失只是连锁反应 |
| 修复 | plain generator override 排除 `CO_ITERABLE_COROUTINE`、`CO_COROUTINE`、`CO_ASYNC_GENERATOR` | 普通 `generators:Tree.__iter__` 仍可放行；asyncio coroutine 不再误编译 |
| 修复验证 | fixed compile-events 共 `2838` 条，无 `asyncio.tasks:__sleep0`；`dask --fast` 通过，正式 `dask=1.77s +- 0.05s` | 功能失败已修复 |

这条链路说明 plain generator 的边界必须按 code flags 判定：`CO_GENERATOR` 只说明实现形态，不说明语义上可按普通 generator 处理。只要带 `CO_ITERABLE_COROUTINE`，它就属于 await 协议的一部分，不能用 `generators` 用例的收益结论泛化放行。

### 17.6 修复后性能状态

| 对比口径 | 旧结果 | 修复后 | 读法 |
|---|---:|---:|---|
| 旧 AutoJIT -> 修复后 AutoJIT | `2.02s` | `1.77s`，`1.15x faster` | 本轮修复和近期 LowRoi 冻结/steady 收窄叠加后，dask 比旧账本更好 |
| `PYTHONJITAUTO=2` -> 修复后 AutoJIT | `2.01s` | `1.77s`，`1.14x faster` | AutoJIT 对 dask 已不是纯负收益，但仍未达标 |
| CPython JIT -> 修复后 AutoJIT | `1.36s` | `1.77s`，`1.30x slower` | 距 CPython JIT 85% 目标仍很远 |
| CinderX plugin-no-JIT -> 修复后 AutoJIT | `1.36s` | `1.77s`，`1.30x slower` | 主差距仍来自启用 CinderX JIT 后的动态成本 |

修复后的 compile-events 摘要：旧 debug 事件为 `3485` 条、`unique=812`；修复后 fixed fast 事件为 `2838` 条、`unique=764`，且 `LowRoi` 长等待路径不再进入 compile-events。被移除的高频编译包括 `asyncio.futures:_set_state/_call_check_cancel`、`distributed.metrics` callback、`distributed.worker:Worker.execute` 等一批框架异步路径。这个方向解释了 `1.15x faster`，但不是 dask 的终局优化：只要仍有大量 scheduler/worker/cloudpickle/zict 动态 deopt，dask 仍会落后 CPython JIT/plugin-no-JIT。

### 17.7 策略判断

| 观察 | 结论 |
|---|---|
| `auto=2` 到当前 AutoJIT 没有稳定收益 | 当前分类器主要解决 startup/import 或低热 compile storm；dask 的成本在 steady 动态执行期 |
| import 编译事件只有 `45/3485` | provider/import window 对 dask 不是主要杠杆 |
| `classified_defer_freeze` 已有 `161135` 次，但仍有 `3408` 次编译和 `1020754` 次 deopt | 静态分类已经挡了一部分低收益函数，但挡不住热到阈值后的动态负 ROI |
| 代表函数是 scheduler/worker/client/asyncio/cloudpickle/zict 的主体路径 | 不能简单全局拦 `BranchFSM/ObjectManipulator/ReflectionMeta + codeB>0`，否则会把 dask 主体工作禁编 |
| `asyncio.tasks:__sleep0` 是 `CO_ITERABLE_COROUTINE` | plain generator 放行规则必须排除 generator-based coroutine；否则会从性能误伤升级为功能失败 |
| deopt 主要来自 slot guard 多态、expected exception、状态机事件多态 | 真正优化路线是 JIT 动态成本专项：slot guard 多态处理、expected exception 慢路径、状态机/序列化路径的 deopt-aware 策略 |
| 当前 RoiBackoff on 后 deopt 已降到百级 | 不能继续把历史百万级 deopt 当作当前主瓶颈；RoiBackoff 已止血，剩余差距需要重新拆账 |
| `PYTHONJITATTRCACHES=0` 同核串行复核 `1.04x faster` | attr cache 是可疑小账，但全局关闭风险过大；只能继续做局部 attr-cache/PIC/站点级泛化实验 |
| LOAD_ATTR fallback-off、STORE fallback、noarray 都没有正式收益 | 不做全局 lowering 回退；这类改动会影响大量单态站点和数组快路径，收益证据不足 |

策略结论：`dask` 应作为 **steady async/framework 动态负 ROI 样本** 进入证据集。当前 AutoJIT v1 不根据 dask 扩大 startup/import 规则，也不根据 dask 默认关闭全局 attr cache 或全局 slot fallback。若后续继续优化 dask，优先级是：

| 优先级 | 方向 | 原因 |
|---|---|---|
| P1 | 局部 attr-cache/PIC/站点级泛化 | noattr 同核串行复核有 `1.04x` 正信号，但只能局部化，不能全局默认 |
| P2 | expected-exception 慢路径专项 | RoiBackoff off 时 `DeleteSubscr`/`VectorCall` 风暴立即出现，说明这类慢路径仍是潜在大账 |
| P3 | 重新拆当前 `1.61-1.68s` 剩余成本 | 当前 deopt 已不大，剩余差距可能在 frame/call 固定成本、attr cache 维护、JIT/解释切换等小账 |
| 暂不做 | 扩大 provider、全局 highcost 静态拦截、全局 LOAD/STORE fallback、noarray | 已有正式或穿刺证据显示不是当前 dask 主杠杆 |

## 18 用例：richards

### 18.1 总体判断

`richards` 是典型 JIT 用例：主体是任务调度器，反复读写 `Task`、`TaskState` 和各类 `TaskRec` 对象状态。它不像 `2to3` 那样慢在 startup/import 编译风暴，也不像 `pickle_pure_python` 那样主要慢在 plugin/frame evaluator 逐帧税；它的问题是 AutoJIT 分类把一批小而热的状态机方法按 `LowRoi` 冻结，导致 CinderX JIT 原本应该拿到的对象状态机收益丢失。

本轮策略只补两个窄口：

- 状态 predicate/mutator：纯状态读、纯状态写、组合布尔状态判断，在 steady-state 低风险小 code 下恢复全局阈值。
- protocol dispatch core：小型 steady 低风险实例方法，有对象状态访问、控制流、返回值，并且有状态写入或 raise 分支；call-only wrapper 仍然延迟或冻结。

最终结果不是“放开所有对象/控制函数”，而是只把 `richards` 任务调度核心放回 JIT，同时避免误伤 `pickle_pure_python` 的 setup/call-only wrapper。

### 18.2 总表：收益与误伤边界

口径：`blue-98:cinderx-test`，`CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 PYTHONJITHUGEPAGES=0`，默认 import/setup provider，`--warmup 3 --affinity=30`。

| 口径 | `richards` | `pickle_pure_python` | `2to3` | `nbody` | 结论 |
|---|---:|---:|---:|---:|---|
| state-helper-only | `124ms` | `802us` | `578ms` | `119ms` | 只修复 trivial/composite state helper，`richards` 主调度函数仍缺失 |
| protocol core 早期版 | `108ms` | `883us` | `577ms` | `111ms` | `richards` 已恢复，但误放 `pickle` call-only/setup wrapper |
| global-cap 早期版 | `111ms` | `903us` | `578ms` | `115ms` | 限制 `LOAD_GLOBAL* <= 4` 仍不足以保护 `pickle` |
| 最终 store-or-raise | `109ms +- 7ms` | `800us +- 3us` | `577ms +- 1ms` | `118ms +- 14ms` | 保留 `richards` 收益，`pickle/2to3` 不劣化；`nbody` 样本不稳 |

相对 state-helper-only，最终策略让 `richards` `1.14x faster`。`pickle_pure_python` 与 `2to3` 没有测出负收益，说明新增放行条件已经收窄到“状态机核心”，没有回到全局放行 `BranchFSM/ObjectManipulator`。

### 18.3 函数形状表

最终策略相对 state-helper-only 新增编译 7 个 benchmark 主体函数：

| 函数 | 形状 | 放行原因 | 判断 |
|---|---|---|---|
| `TaskState.isTaskHoldingOrWaiting` | `BranchFSM`, `loop=0`, `codeB=0`, `risk=0`, dims=`Control+Object` | composite state predicate：多状态位组合布尔判断 | 应恢复 `2/None`；这是任务调度状态查询 |
| `TaskState.isWaitingWithPacket` | `BranchFSM`, `loop=0`, `codeB=0`, `risk=0`, dims=`Control+Object` | composite state predicate：多状态位组合布尔判断 | 应恢复 `2/None` |
| `Task.addPacket` | `Mixed`, `loop=0`, `codeB=0`, `risk=0`, dims=`Control+Object+Dispatch` | protocol core：读写队列/状态并返回下一个 task | 应恢复 `2/None` |
| `Task.findtcb` | `BranchFSM`, `loop=0`, `codeB=0`, `risk=0`, dims=`Control+Dispatch+Dynamic` | protocol core：查表失败走 raise 分支，属于协议约束 | 应恢复 `2/None`，但 `LOAD_GLOBAL*` 必须有上限 |
| `DeviceTask.fn` | `BranchFSM`, `loop=0`, `codeB=0`, `risk=0`, dims=`Control+Object+Dispatch+Dynamic` | protocol core：按 packet/state 分派并写 `pending` | 应恢复 `2/None` |
| `HandlerTask.fn` | `BranchFSM`, `loop=0`, `codeB=1`, `risk=0`, dims=`Control+Object+Dispatch` | protocol core：消费 work/device 队列，更新内部状态 | 应恢复 `2/None` |
| `IdleTask.fn` | `ObjectManipulator`, `loop=0`, `codeB=1`, `risk=0`, dims=`Control+Object+Dispatch` | protocol core：更新计数和控制位，调度下一个 task | 应恢复 `2/None` |

### 18.4 负面边界

| 边界 | 原因 |
|---|---|
| 不放行 `__init__` | 构造函数容易出现在 setup/import 阶段，且对 steady 调度循环收益不直接 |
| 不放行 call-only wrapper | `pickle_pure_python` 证明只看对象/控制/调用会误放 setup 和 harness wrapper |
| `LOAD_GLOBAL*` 必须有限制 | `Task.findtcb` 需要少量全局异常/表访问，但全局探测过多更像环境/setup 逻辑 |
| 必须有状态写入或 raise 分支 | 区分真正的状态机 protocol core 与普通包装/转发函数 |

## 19 当前策略判断汇总

| 规则 | 支撑用例 | 结论 |
|---|---|---|
| `highcost > 0` 包含 `>= 2` | `2to3` | 成立。直接 JIT debug 口径显示 `>0` 相比 `>=2` 进一步减少编译数和编译耗时；pyperformance 墙钟噪声不能用来否定集合包含关系 |
| import window 内高成本非数值形状应延迟 | `2to3`、`coverage` startup=true 样本 | 成立。目标是削减 startup/import compile storm |
| steady-state highcost 不能全局延迟 | `unpack_sequence` | 成立。`do_unpacking` 是 `ObjectManipulator + codeB=3 + risk=HugeCode`，但必须放行 |
| suspendable 不能一刀切禁止 | `generators` | 成立。`Tree.__iter__` 是 steady-state 普通 generator；`yield from` cleanup 带来的 `Exception` 风险不能触发解释冻结，修复后恢复 `2/None` |
| `CO_ITERABLE_COROUTINE` 不能按普通 generator 放行 | `dask` | 成立。`asyncio.tasks:__sleep0` 同时带 `CO_GENERATOR` 和 `CO_ITERABLE_COROUTINE`，误编译会让 `asyncio.sleep(0)` 抛 `TypeError: 'generator' object can't be awaited` |
| steady 低风险状态 predicate/mutator 不能按 LowRoi 冻结 | `richards` | 成立。`TaskState` 状态查询/状态写入是任务调度核心，小 code 低风险时应恢复全局阈值 |
| protocol dispatch core 需要窄放行 | `richards`、`pickle_pure_python` | 成立。`richards` 需要放行有状态写入或 raise 分支的任务调度核心；`pickle_pure_python` 证明 call-only/setup wrapper 不能一起放开 |
| `compute=true` 默认放行 | `coverage:fibonacci`、`generators:tree` | 成立。数值/compute 提示是收益信号 |
| coverage 回归不能直接驱动 import-window 策略收窄 | `coverage` | 成立。其高成本函数多在 steady worker，属于单独 ROI 问题 |
| 框架型 steady highcost 需要单独 ROI 判断 | `sqlalchemy_declarative`、`sqlalchemy_imperative` | 成立。ORM/engine 热路径里存在 `BranchFSM`、`ObjectManipulator`、`ReflectionMeta` 大函数；`sqlalchemy_declarative` 复跑确认 plugin-no-JIT 已达标，而 CinderX JIT/AutoJIT 都是负 ROI |
| `LowRoi` 延迟不是禁编，会产生测量窗口长尾 | `sqlalchemy_declarative` | 成立。当前 AutoJIT median `377.5ms` 好于 `auto=2`，但 mean `434.4ms` 被 `458-626ms` 慢值拉高；`classified_warmup_return=358240` 后仍有 `2127` 次编译 |
| parser/optimizer 树处理不能按 `ObjectManipulator + codeB=2` 全局延迟 | `sqlglot_v2` 系列 | 成立。`Parser._parse`、`pushdown_projections`、`optimize_joins` 等是 benchmark 主体工作 |
| `StartupInit` 是阶段条件，不是单独拦截理由 | `sqlalchemy`、`sqlglot` startup 样本 | 成立。只有 startup/import 内高成本、非数值、非 compute 形状才是目标；小函数和 compute 函数不应被误伤 |
| `LowRoi` 是热度延迟，不是禁编 | `logging_simple`、`logging_format`、`sympy_expand`、`sympy_str` | 成立。日志输出和符号计算核心函数会在 gate 数足够高后编译 |
| highcost 符号计算不能全局延迟 | `sympy` 系列 | 成立。`Mul.flatten`、`Expr.expand`、`simplify`、`Printer._print` 等是主体工作，不是 startup/import 噪声 |
| benchmark-self 优先于全局 top 编译耗时 | `logging` 系列 | 成立。全局 top 可能混入 `importlib.metadata`、`argparse`、driver/setup 函数，策略判断必须回到本体函数 |
| pure-python 对象图序列化不能全局拦 `BranchFSM + codeB=2` | `pickle_pure_python` | 成立。`save_tuple/save_global/_batch_*` 是主体路径，LowRoi 延迟后仍编译 |
| plugin/frame evaluator 逐帧税可以压倒 JIT 收益 | `pickle_pure_python` | 成立。no-plugin `643.8us`，plugin-no-JIT/`auto=2`/AutoJIT 都约 `1.06ms`；主差距不是分类策略 |
| 对象图重建热点应允许热后编译 | `deepcopy` | 成立。`copy._reconstruct` 延迟到 1000 后仍进入 JIT，内部热点优先于入口函数 |
| expected-exception deopt 需要精确到函数形状 | `deepcopy` | 成立。`_deepcopy_tuple` 抑制后正式 A/B 有小幅收益；`_keep_alive` 放宽会让 `deepcopy_reduce` 回归，应保持 `RiskDefer` |
| deopt 数不能单独驱动准入策略 | `deepcopy` | 成立。必须同时看函数形状、子场景和正式 A/B；否则会误伤 `deepcopy_memo` |
| `RiskDefer` 对低热大函数有效，但不能替代收益判断 | `pickle_pure_python`、`deepcopy` | 成立。部分入口/通用函数未编译是保护，但如果主差距在逐帧税或特定 deopt 上，继续调 `RiskDefer` 不会解决问题 |
| steady async/framework 动态负 ROI 不能靠 startup/import 泛化解决 | `dask`、`sqlalchemy_declarative` | 成立。`dask` 的主因是 `distributed`/`asyncio`/`cloudpickle`/`zict` deopt；`sqlalchemy_declarative` 的主因是 ORM steady guard failure 加 delayed compile 长尾 |
| 进程池固定成本窗口需要 setup provider 覆盖 | `bench_mp_pool` / `concurrent_imap` | 成立。用例主体是 `Pool(2).imap(f, range(1000), chunk=10)`，`f(x)=x` 几乎没有 JIT 计算收益；策略目标是让 Pool bootstrap 和 job submission 不制造低阈值 AutoJIT 固定成本。result iterator 消费是 timed 动态路径，不能放进 setup wrapper |

## 20 `bench_mp_pool` / `concurrent_imap` 初始账本

### 20.1 用例模型

| 项 | 内容 | 策略含义 |
|---|---|---|
| pyperformance 名称 | `concurrent_imap`，结果行 `bench_mp_pool` | 证据表和命令中需要同时标注两个名称，避免把 selector 与结果行混淆 |
| benchmark 主体 | `with Pool(2) as pool: for _ in pool.imap(f, range(1000), 10): pass` | 主要成本是 Pool/forkserver、IPC、pickle、queue、result 消费和调度唤醒 |
| worker 函数 | `f(x): return x` | 典型 `Trivial`/低 ROI 函数，正常不应进入 JIT |
| JIT 收益来源 | 很少 | 不能按数值循环用例期待 JIT 正收益 |
| AutoJIT 目标 | 减少额外成本 | 让进程池初始化/通信窗口走 startup/setup 保守策略，并确认 trivial/infra 函数不被误编译 |

### 20.2 五口径总表

目标容器口径：blue-98 临时 4C16G 容器，`--cpuset-cpus=0-3 --memory=16g`，同 NUMA，不额外 `--affinity`；pyperformance `--warmups 3`；candidate worker venv `include-system-site-packages=true`。本轮 `/opt/python314` 的 `sys._jit.is_available() == False`，因此表内 baseline 是 no-plugin 解释器地板，不是 CPython JIT 发布基线；正式 CPython JIT 85% 线仍需在目标发布环境回灌。

| 口径 | 命令差异 | `bench_mp_pool` mean / median | 相对 no-plugin | `bench_thread_pool` mean | 主要解释 |
|---|---|---:|---:|---:|---|
| no-plugin baseline | 不加载 CinderX plugin | `120.0ms / 101.0ms` | `1.00x` | `1.613ms` | 4C 进程池本身抖动大，std `67.9ms`；作为本轮环境地板 |
| CinderX plugin-no-JIT | `CINDERX_PLUGIN_ENABLE=1 PYTHONJITDISABLE=1` | `117.8ms / 96.1ms` | `0.98x` | `1.630ms` | plugin 固定成本不是主因；未启用 JIT 时基本贴近 no-plugin |
| CinderX `PYTHONJITAUTO=2` | 传统低阈值 JIT | `950.8ms / 972.8ms` | `7.92x` | `1.884ms` | 低阈值 JIT 在 Pool/bootstrap/infra 路径触发编译风暴和动态成本，是原始劣化主因 |
| AutoJIT，setup provider off | `PYTHONJITAUTO=auto:2 CINDERX_AUTOJIT_SETUP_PROVIDER=off` | `133.7ms / 125.6ms` | `1.11x` | `1.734ms` | AutoJIT 分类、LowRoi/RoiBackoff 已压掉大头，但仍有约 `+13.7ms` 残差 |
| AutoJIT，错误 iterator wrap | `Pool.imap` + `IMapIterator.next` 都包 setup | `141.8ms / 128.6ms` | `1.18x` | `1.820ms` | 逐 result `next` 进入 Python wrapper，timed 消费路径被拖慢，证明 result iterator 不能包 |
| AutoJIT，最终 Pool.imap-only provider | 默认 provider：包 `Pool.imap`，不包 iterator/get | `126.9ms / 112.6ms` | `1.06x` | `1.752ms` | 相对 provider off 再拿回约 `6.8ms` mean / `13.0ms` median；接近 plugin/no-plugin 地板 |

### 20.3 阶段拆分表

| 阶段 | 触发频率 | 证据字段 | 当前策略 |
|---|---|---|---|
| Pool 构造 / worker 启动 | 每次 `Pool(2)` | compile-events `phase=setup`、gate stats、worker pid 分布 | `multiprocessing_pool` provider 包 `Pool.__init__` |
| Pool context 生命周期 | 每次 `with Pool(...)` | setup depth smoke、compile-events `phase=setup` | `Pool.__enter__` 进入 setup depth，`Pool.__exit__` 退出 |
| 同步/异步提交 | 每次提交 | compile-events `phase=setup` | `Pool.map/imap/imap_unordered/starmap/map_async/starmap_async` 用 setup wrapper |
| imap 结果消费 | 每次 result iterator `next` | `bench_mp_pool` 主体耗时、thread_pool 对照 | **不包装**。这是 timed 动态路径；错误包装会把最终 mean 从 `126.9ms` 拖到 `141.8ms`，并把 `bench_thread_pool` 从 `1.75ms` 拖到 `1.82ms` |
| async result get | 每次 `ApplyResult.get` | async result 消费路径 | **不包装**。`get()` 是等待/消费结果，不是提交窗口；后续若有 async 专项需要单独取证 |
| worker 函数 `f` | 每个任务 | compile-events/gate stats 中 `fullname`、`family=Trivial` | 走 LowRoi/trivial 冻结，不应 forced compile |

### 20.4 需要回灌的函数形状表

| 函数/模块 | 预期形状 | 预期策略 | 验收 |
|---|---|---|---|
| benchmark `f` | `Trivial`、`loop=0`、`risk=0` | `LowRoi` 冻结或保持解释执行 | `forced_compile=0` |
| `multiprocessing.pool.Pool.*` wrapper 命中的 helper | 多为 `CallDispatcher` / `ObjectManipulator` / `BranchFSM`，非 compute-dominant | setup 窗口内延迟 | `Pool.imap` 有 provider marker，`IMapIterator.next` 无 marker；高成本非数值不编译或显著减少 |
| import/forkserver bootstrap helper | import/setup 阶段函数 | import/setup 延迟 | 不扩大到 steady worker 主体 |
| `ThreadPool` 路径 | 不属于本 provider | 不进入 `multiprocessing_pool` setup depth | `ThreadPool` 继承 wrapped `Pool.imap` 时仍经过 wrapper 函数，但 exact `type(self).__name__ == "Pool"` 谓词返回 false，setup depth 保持 0 |

## 21 待补清单

| 优先级 | 项 | 目的 |
|---|---|---|
| P0 | 生成当前 `highcost > 0` 口径下 `2to3` 的完整 shape TSV | 让 `2to3` 主目标与当前策略完全对齐 |
| P0 | 将 `2to3` 拆成 `import lib2to3.main` 与 `main() refactor` 两阶段函数表 | 区分 import-window 应延迟与 steady/refactor 应放行 |
| P1 | 给 `coverage` 补非 debug 性能 A/B 与候选级动态收益 | 判断是否需要 steady coverage 专项策略，而不是污染 import-window 策略 |
| P1 | 给 `sqlalchemy_imperative` 补同格式表 | `sqlalchemy_declarative` 已补当前五口径账本；imperative 仍需按同样口径拆分 |
| P1 | 对 `sqlalchemy_declarative` top deopt/LowRoi 函数做严格禁编穿刺 | 验证把部分 ORM 多态路径从“延迟后仍编译”改成“不编译/动态禁编”能否拿回 mean 长尾 |
| P1 | 给 `sqlglot_v2` 四个子用例分别补非 debug A/B | 区分 parse/transpile/optimize 的真实收益和误伤边界 |
| P1 | 给 `logging_simple` / `logging_format` 补更细的 codegen/调用路径分析 | `logging_silent` 已通过 defer predicate 与 call-only loop 达标；输出路径仍比 CPython JIT 慢约 1.2x，需要单独判断是否值得做 JIT codegen 优化 |
| P1 | 给 `sympy` 四个子用例分别补非 debug A/B | 判断符号计算 highcost 函数的动态收益是否覆盖编译成本 |
| P1 | 拆 `pickle_pure_python` 正式 AutoJIT 残留成本 | auto 模式已不装 frame evaluator 且 LowRoi 冻结后达到 85% 线；后续只需继续拆 gate/compiled-entry/startup hook 的约 `156us` 残留上界 |
| P1 | 将 `deepcopy/deepcopy_reduce/deepcopy_memo` 三个子场景补完整函数形状表 | deopt 和正式 A/B 已拆分；后续还需要按子场景列完整编译函数，确认 `_deepcopy_tuple` 收窄规则是否有其它同形状候选 |
| P1 | 给 `dulwich_log` 补同格式表 | 扩大非 JIT 用例样本，避免只围绕 `2to3`/`dask` 调参 |
| P1 | 将 `bench_mp_pool` 五口径正式数值和函数形状回灌到 §20 | 验证 `multiprocessing_pool` provider 是否把 4C16G no-affinity 口径恢复到 CPython JIT 85% 线 |
| P2 | 给 `scimark_fft/scimark_lu/scimark_sor/scimark_monte_carlo` 补同格式表 | 验证 JIT 用例误伤边界 |
