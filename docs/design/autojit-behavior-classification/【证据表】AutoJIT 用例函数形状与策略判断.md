# AutoJIT 用例函数形状与策略判断证据表

## 1 文档说明

本文档独立维护 AutoJIT 行为分类策略的用例级证据表。它不替代需求、功能设计或详细设计；它回答一个更具体的问题：**某个 pyperformance 用例里，CinderX JIT 实际编译了哪些函数，这些函数落在什么结构形状上，当前策略应该放行、延迟还是继续收窄。**

后续调策略时先读这张表。任何新增样本必须按同一口径补充：用例、测试口径、数据来源、函数列表、函数形状、当前策略、策略判断。

## 2 修订记录

| 版本 | 日期 | 修订人 | 修订说明 |
|---|---|---|---|
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
| 纯 `ObjectManipulator` 大函数 | import window 可延迟；steady-state 中可能是核心热点，不能全局拦 |
| `RiskDefer` | 只说明风险成本高；上线前必须证明省下的静态成本大于丢掉的动态收益 |

## 5 口径索引

| 用例 | 口径 | 数据来源 | 说明 |
|---|---|---|---|
| L3 典型子集 | A/B：baseline=`PYTHONJITAUTO=2`，candidate=`PYTHONJITAUTO=auto:2` + `CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` + `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main`，`--warmup 3 --affinity=30` | `blue-98:cinderx-test:/results/autojit-l3-subset-compute-provider-20260605/{baseline-auto2.json,candidate-auto-classify-provider.json,compare-table.txt}` | 同一 candidate wheel 内比较分类策略开/关；17 个 selector 展开为 25 个 compare row；几何均值 1.13x faster，主要由 `2to3`/startup 拉动 |
| `2to3` | `PYTHONJITAUTO=auto:2`，早期 `size100` 策略形状表 | `blue-98:/results/autojit-compile-lists-20260605/2to3.shape.tsv` | 有完整函数形状，共 122 个编译事件；不是最终 `highcost > 0` 口径 |
| `2to3` | 当前 `highcost > 0` import-window 策略 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/2to3-direct-debug.jit.log` | 有编译数和编译耗时，完整形状表待补 |
| `2to3` | compute-dominant 修正，无 setup provider，`PYTHONJITAUTO=auto:2` | `blue-98:/results/autojit-compute-dominant-20260605/2to3-candidate.json`；debug: `.../current-2to3-debug/2to3.jit.log` | 正式均值 1.315s；debug 158 个编译事件、累计 483.207ms |
| `2to3` | runpy 原型：整个 `lib2to3` main/refactor 窗口复用现有 depth | `blue-98:/results/autojit-compute-dominant-20260605/prototype-main-window-debug/2to3-main-window.jit.log` | debug 118 个编译事件、累计 187.628ms；证明需要 setup/main window 数据源 |
| `2to3` | 真实 `python -m lib2to3`，`CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main` | `blue-98:/results/autojit-compute-dominant-20260605/2to3-setup-provider.json`，final: `.../2to3-setup-provider-final.json`；debug: `.../setup-provider-debug/2to3-setup-provider.jit.log` | 正式均值 0.968s；final wheel 复跑 0.965s 但 pyperf 标记样本稳定性 warning；debug 122 个编译事件、累计 196.548ms |
| `python_startup` | `PYTHONJITAUTO=auto:2` | `blue-98:/results/autojit-compile-lists-20260605/python_startup.shape.tsv` | 有完整函数形状，共 2 个编译事件 |
| `coverage` | direct worker，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/coverage.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `generators` | direct worker，同上 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/generators.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `unpack_sequence` | direct worker，同上 | `blue-98:/results/autojit-import-highcost-bucket1-20260605/worker-gate-shapes/unpack_sequence.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
| `sqlalchemy_declarative` | direct worker，`--fast --values=3 --warmups=1`，`PYTHONJITAUTO=auto:2`，`CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlalchemy_declarative.*.jit.log` | 有真实 C++ gate 形状；debug 口径，不作为性能数值 |
| `sqlalchemy_imperative` | direct worker，同上 | `blue-98:/results/autojit-sql-shapes-20260605/worker-gate-shapes/sqlalchemy_imperative.*.jit.log` | 有真实 C++ gate 形状；debug 口径 |
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
| `deepcopy` | direct worker，同上，`bm_deepcopy` 一次 run 覆盖 `deepcopy/deepcopy_reduce/deepcopy_memo` | `blue-98:/results/autojit-pickle-deepcopy-shapes-20260605/worker-gate-shapes/deepcopy.*.jit.log` | 有真实 C++ gate 形状；debug 口径；函数表按 benchmark-self 归因 |

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
| 轻微回归/待查 | `sqlalchemy_declarative` | 387ms -> 451ms，1.16x slower | candidate 方差 94ms，先复跑确认再定策略 |
| 轻微回归/待查 | `logging_silent` | 363ns -> 420ns，1.16x slower | 纳秒级用例，可能是测量噪声或低 ROI 延迟的固定成本 |
| 噪声/基本持平 | `dask`、`deepcopy_reduce` | hidden as not significant | 暂不作为策略调整依据 |

总体几何均值为 1.13x faster，但主要由 `2to3` 和 startup 拉动。下一步策略优化不应再围绕 startup/import 风暴泛化，而应针对 steady-state 轻微回归样本复跑 gate log，确认是否存在过度 `LowRoi/RiskDefer`。

## 6 用例：2to3

### 6.1 当前总体判断

`2to3` 是非 JIT 用例优化主目标。它的主要问题不是某个热数值循环慢，而是启动/import 与 refactor 初始化过程中大量 `lib2to3`、`optparse`、`logging`、`difflib` 函数在低阈值下被推入 JIT。`size100` 口径下 122 个编译事件、累计编译耗时 420.592ms；`highcost > 0` import-window 策略下 156 个编译事件、累计编译耗时 488.426ms。

2026-06-05 的 compute-dominant 复跑说明：仅把 incidental `Compute` 从保护条件中移除有收益，但不是决定性收益。无 setup provider 时，真实 pyperformance `2to3` 为 1.315s，debug 口径仍有 158 个编译事件、累计 483.207ms，top 编译函数仍集中在 `lib2to3` refactor/pattern 阶段。把整个 `lib2to3` main/refactor 窗口标成 setup window 后，真实 `python -m lib2to3` debug 降到 122 个编译事件、196.548ms，正式 pyperformance 降到 0.968s。结论是：继续优化 `2to3` 不能只调静态分类，必须引入能覆盖 main/refactor 初始化窗口的新数据源；import depth 只能覆盖 import 阶段。

### 6.2 size100 口径族分布

| family | 编译函数数 |
|---|---:|
| `ObjectManipulator` | 48 |
| `BranchFSM` | 41 |
| `Trivial` | 12 |
| `Mixed` | 4 |
| `ReflectionMeta` | 3 |
| `NumericLoop` | 3 |
| `CallDispatcher` | 1 |

### 6.3 size100 口径策略分布

| 策略 | 编译函数数 | 判断 |
|---|---:|---|
| `2/None` | 81 | 沿用全局阈值，说明这些函数按当时策略被放行 |
| `1000/LowRoi` | 19 | 已识别为低 ROI 延迟；热度足够时仍可能编译 |
| `4/LowRoi` | 12 | Trivial 轻延迟 |

### 6.4 关键函数形状表

| 函数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---|---|---|
| `lib2to3.btm_utils:reduce_tree` | 41.944ms | `BranchFSM`, dims=`Control+Object+Dynamic+Compute+Dispatch`, `loop=3`, `codeB=2`, `risk=HugeCode` | `1000/LowRoi` | 应延迟；这是高成本分支/对象遍历，不应在 import window 低阈值马上编译 |
| `lib2to3.fixes.fix_metaclass:FixMetaclass.transform` | 33.650ms | `ObjectManipulator`, dims=`Object+Control+Dispatch+Dynamic`, `loop=1`, `codeB=2`, `risk=HugeCode` | `2/None` | 当前表明会放行；若发生在 import window，属于高成本非数值候选，应由 `highcost > 0` import-window 策略延迟 |
| `lib2to3.patcomp:PatternCompiler.compile_node` | 33.185ms | `ObjectManipulator`, dims=`Object+Control+Compute+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode` | `2/None` | 当前形状带 `Compute` 计数但不是数值主导；是否延迟要看 import window 和动态收益，不能只按 family 猜 |
| `lib2to3.fixes.fix_raise:FixRaise.transform` | 14.428ms | `ObjectManipulator`, dims=`Object+Control+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=HugeCode` | `2/None` | import window 内应倾向延迟；steady refactor 热路径需继续看收益 |
| `difflib:_check_types` | 11.322ms | `ReflectionMeta`, dims=`Dynamic+Control+Dispatch+Compute+Object`, `loop=1`, `codeB=2`, `risk=Dynamic` | `2/None` | 动态/反射成本高，import/setup 内可延迟；steady 中需证据 |
| `lib2to3.pytree:WildcardPattern._recursive_matches` | 11.174ms | `BranchFSM`, dims=`Control+Object+Dispatch+Compute+Suspend`, `loop=3`, `codeB=1`, `risk=None` | `2/None` | 递归匹配可能是 refactor 热点，不能全局拦 |
| `lib2to3.pytree:WildcardPattern.generate_matches` | 10.637ms | `BranchFSM`, dims=`Control+Object+Dynamic+Dispatch`, `loop=3`, `codeB=2`, `risk=Exception` | `1000/LowRoi` | 延迟合理；热度足够时仍可编译 |
| `lib2to3.pytree:generate_matches` | 9.426ms | `BranchFSM`, dims=`Control+Object+Dispatch+Suspend+Compute`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | 可能是匹配热路径，不能只因非数值拦 |
| `lib2to3.fixes.fix_import:traverse_imports` | 8.252ms | `ObjectManipulator`, dims=`Object+Control+Compute+Dispatch+Dynamic+Suspend`, `loop=3`, `codeB=2`, `risk=Exception` | `2/None` | import/refactor 交界函数；需要按阶段区分 |
| `lib2to3.pytree:NodePattern._submatch` | 7.952ms | `BranchFSM`, dims=`Control+Object+Dispatch+Dynamic+Compute`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | refactor 热路径候选，不应被 import-only 策略误伤 |
| `optparse:Option._set_opt_strings` | 7.384ms | `BranchFSM`, dims=`Control+Compute+Dispatch+Object+Dynamic`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | 启动配置解析，import/startup 内应延迟；steady 不常见 |
| `lib2to3.btm_utils:MinNode.leaf_to_root` | 7.262ms | `ObjectManipulator`, dims=`Object+Control+Dispatch+Dynamic+Compute`, `loop=3`, `codeB=2`, `risk=None` | `2/None` | 模式树遍历，可能有动态收益；需阶段证据 |
| `difflib:SequenceMatcher.get_opcodes` | 6.547ms | `BranchFSM`, dims=`Control+Object+Compute+Dispatch`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | 稳态可能收益，不能全局拦 |
| `lib2to3.fixes.fix_metaclass:has_metaclass` | 6.482ms | `BranchFSM`, dims=`Control+Object+Dynamic+Compute+Dispatch`, `loop=3`, `codeB=1`, `risk=None` | `2/None` | refactor 热路径候选 |
| `lib2to3.pytree:NodePattern.__init__` | 6.120ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic+Object+Compute`, `loop=2`, `codeB=1`, `risk=None` | `2/None` | 初始化形状，import/setup 内可延迟 |
| `lib2to3.pgen2.parse:Parser.pop` | 5.853ms | `ObjectManipulator`, dims=`Object+Control+Dispatch+Compute`, `loop=0`, `codeB=0`, `risk=None` | `2/None` | 小对象操作，当前不作为重点拦截对象 |
| `lib2to3.btm_utils:MinNode.leaves` | 3.708ms | `BranchFSM`, dims=`Control+Suspend+Object+Dispatch`, `loop=2`, `codeB=0`, `risk=Exception`, `suspend=true` | `1000/LowRoi` | 延迟合理；不能一刀切禁编 generator/suspend 类路径 |

### 6.5 当前最终策略待补

| 缺口 | 需要补的数据 |
|---|---|
| 当前 setup provider 完整函数形状表 | 基于 `/results/autojit-compute-dominant-20260605/setup-provider-debug/2to3-setup-provider.jit.log` 生成同字段 TSV：函数、编译耗时、family、dims、loop、codeB、risk、active dims、limit/reason |
| import lib2to3 阶段 vs `main()` refactor 阶段 | 当前证据已证明 `main()` refactor 阶段是主要剩余编译风暴来源；后续需要把阶段字段固化到 debug TSV |
| 剩余 122 个 setup-provider 编译事件 | top 已降为 `Parser.pop`、`Logger.makeRecord`、`Parser.shift`、`WildcardPattern.match_seq` 等 3-6ms 小函数；需判断是否继续按 setup window 延迟低成本对象/分支形状，或接受 interpret-only 水平 |

## 7 用例：python_startup

### 7.1 总体判断

当前形状表只看到 2 个编译函数，均为启动期小型 `ObjectManipulator`。这说明 `python_startup` 的剩余差距不能只用“编译函数很多”解释，还需要继续分解 CinderX plugin 初始化、frame evaluator、import provider 包装和启动路径本身的固定成本。

### 7.2 函数形状表

| 函数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---|---|---|
| `_sitebuiltins:_Printer.__init__` | 3.376ms | `ObjectManipulator`, dims=`Object+Control+Dispatch+Dynamic`, `loop=2`, `codeB=0`, `risk=None`, `synthetic=true` | `2/None` | 小规模启动函数；不是当前 compile storm 主因 |
| `_frozen_importlib:_ModuleLockManager.__exit__` | 1.552ms | `ObjectManipulator`, dims=`Object+Dispatch+Control`, `loop=0`, `codeB=0`, `risk=None`, `synthetic=true` | `2/None` | 小规模 import 管理函数；继续优化时应先看固定启动成本 |

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

`generators` 是 JIT 用例误伤检查样本。它的核心收益来自递归构造和 generator 遍历，尤其 `Tree.__iter__`。证据显示 `Tree.__iter__` 被分类为 suspendable `BranchFSM`，当前策略把阈值提高到 1000，但不会拦死；热度足够后仍编译。这个结论很重要：**suspendable 可以延迟，但不能一刀切禁止。**

### 9.2 函数形状表

| 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---:|---:|---|---|---|
| `__main__:Tree.__iter__` | 7 | 35.4ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=0`, `risk=Exception`, `suspend=true`, `startup=false`, `compute=false` | `1000/LowRoi` | 延迟但允许热后编译；不能拦死 |
| `__main__:bench_generators` | 7 | 27.7ms | `CallDispatcher`, dims=`Control+Dispatch+Dynamic`, `loop=2`, `codeB=1`, `risk=None`, `startup=false` | `2/None` | 放行；驱动循环有热度 |
| `__main__:tree` | 7 | 21.6ms | `NumericLoop`, dims=`Compute+Control+Dispatch+Dynamic`, `loop=0`, `codeB=0`, `risk=None`, `compute=true`, `startup=false` | `2/None` | 放行；compute 提示明确 |
| `__main__:Tree.__init__` | 7 | 4.8ms | `ObjectManipulator`, dims=`Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | 小对象构造，放行 |

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

`sqlalchemy` 系列是 ORM/框架型用例，不是单纯 startup/import 风暴。真实 worker gate 证据显示，大量 gate 事件落在 `BranchFSM`、`CallDispatcher`、`Mixed`、`ReflectionMeta` 上；其中一部分是 import/setup 阶段的装饰器、签名生成、事件注册，另一部分是 steady 阶段反复执行的 ORM persistence、engine context、SQL compiler。

因此它支撑两个策略边界：第一，`startup=true` 的 import/setup 高成本非数值函数可以延迟；第二，steady-state 的 ORM/engine 热路径不能只因 `codeB` 大或 `risk` 非空就全局粗拦，否则会把真实工作负载一起挡掉。

### 11.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `sqlalchemy_declarative` | 2775 | 635 | 445 | 451516 | 1335 | `BranchFSM:353999`, `CallDispatcher:41218`, `Mixed:34974`, `ReflectionMeta:18039` | `LowRoi:436003`, `RiskDefer:12105`, `None:1818`, `StartupInit:1590` |
| `sqlalchemy_imperative` | 1645 | 370 | 189 | 217927 | 902 | `BranchFSM:143769`, `Mixed:28353`, `CallDispatcher:24464`, `ReflectionMeta:19105` | `LowRoi:206615`, `RiskDefer:8698`, `StartupInit:1560`, `None:1054` |

### 11.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `sqlalchemy_declarative` | `sqlalchemy.orm.persistence:_emit_insert_statements` | 4 | 408.3ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | steady ORM 写入热路径；延迟到高热度合理，但不能 import-only 粗拦 |
| `sqlalchemy_declarative` | `sqlalchemy.orm.loading:_instance_processor` | 4 | 386.5ms | `ReflectionMeta`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false` | `2/None` | 高成本反射/对象处理；若有回归，应做 steady ROI 专项，而不是扩大 import window |
| `sqlalchemy_declarative` | `sqlalchemy.engine.default:DefaultExecutionContext._init_compiled` | 4 | 216.1ms | `ObjectManipulator`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `2/None` | engine 执行上下文核心路径；当前放行，需要用非 debug A/B 判断收益 |
| `sqlalchemy_declarative` | `sqlalchemy.util.deprecations:deprecated_params.<locals>.decorate.<locals>.warned` | 4 | 67.5ms | `CallDispatcher`, dims=`Object+Dispatch+Dynamic`, `loop=0`, `codeB=2`, `risk=None`, `startup=true` | `2097152/StartupInit` | import/setup 装饰器包装，延迟正确 |
| `sqlalchemy_declarative` | `__main__:bench_sqlalchemy` | 4 | 44.3ms | `CallDispatcher`, dims=`Object+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |
| `sqlalchemy_imperative` | `sqlalchemy.engine.default:DefaultExecutionContext._init_compiled` | 4 | 219.1ms | `ObjectManipulator`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `2/None` | 与 declarative 相同，属于 steady engine 热路径 |
| `sqlalchemy_imperative` | `sqlalchemy.engine.base:Connection._execute_context` | 4 | 148.0ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | SQL 执行路径；低 ROI 延迟合理，但不应禁止 |
| `sqlalchemy_imperative` | `sqlalchemy.util.langhelpers:format_argspec_plus` | 5 | 125.4ms | `NumericLoop`, dims=`Compute+Control`, `loop=0`, `codeB=2`, `risk=HugeCode`, `startup=true`, `compute=true` | `2/None` | 启动期但有 compute 提示；验证了 import 策略不能拦 `compute=true` |
| `sqlalchemy_imperative` | `__main__:bench_sqlalchemy` | 4 | 39.2ms | `CallDispatcher`, dims=`Control+Object+Dispatch+Dynamic`, `loop=2`, `codeB=1`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |

### 11.4 策略判断

| 观察 | 结论 |
|---|---|
| `BranchFSM` 占 gate 主体，且多数 `LowRoi` 来自 ORM/engine steady 热路径 | 不能把 `BranchFSM + codeB>0` 解释成“一律低收益”；只能提高低 ROI 阈值，让热度证明自己 |
| `deprecated_params...warned` 这类 `startup=true` 装饰器包装被 `StartupInit` 延迟 | import/setup 延迟条件有效，适合拦启动期框架初始化 |
| `format_argspec_plus` 是 `startup=true` 但 `compute=true` | startup/import 策略必须保留 compute 例外 |
| benchmark 本体 `bench_sqlalchemy` 为 `CallDispatcher` 且 `limit=2` | 本体驱动函数不应被 import-window 策略误伤 |

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

`logging` 系列分三类：`silent` 是关闭 debug 日志时的极小热路径，`simple` 和 `format` 是实际输出日志时的对象构造、调用链和格式化路径。真实 worker gate 证据显示，三类用例的 gate 主体都是 `BranchFSM`；但 `silent` 的 benchmark 本体和 `Logger.debug` 很小，应放行，输出类用例里的 `LogRecord.__init__`、`Logger.callHandlers`、`Logger.findCaller` 等 steady 热路径被 `LowRoi` 延迟到 1000 后仍会编译。

因此 `logging` 支撑一个很具体的策略边界：`LowRoi` 是“先解释执行，热度足够再编译”，不是禁编。日志输出路径虽然不是数值循环，但可能是 benchmark 主体；不能把 steady 的日志高成本函数纳入 import-window 拦截。

### 13.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `logging_silent` | 620 | 159 | 3 | 41907 | 480 | `BranchFSM:31938`, `CallDispatcher:3962`, `ReflectionMeta:3477`, `Mixed:1592` | `LowRoi:39660`, `RiskDefer:1664`, `None:423`, `StartupInit:160` |
| `logging_simple` | 704 | 180 | 19 | 101155 | 505 | `BranchFSM:75427`, `CallDispatcher:19702`, `ReflectionMeta:3480`, `Mixed:1592` | `LowRoi:98868`, `RiskDefer:1684`, `None:443`, `StartupInit:160` |
| `logging_format` | 708 | 180 | 19 | 101211 | 506 | `BranchFSM:75474`, `CallDispatcher:19701`, `ReflectionMeta:3482`, `Mixed:1592` | `LowRoi:98924`, `RiskDefer:1684`, `None:443`, `StartupInit:160` |

### 13.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `logging_silent` | `__main__:bench_silent` | 4 | 30.4ms | `CallDispatcher`, dims=`Object+Dispatch`, `loop=1`, `codeB=1`, `risk=None`, `startup=false` | `2/None` | benchmark 本体，应放行 |
| `logging_silent` | `logging:Logger.debug` | 4 | 7.1ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | disabled debug 热路径很小，放行合理 |
| `logging_silent` | `logging:Logger.isEnabledFor` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Object`, `loop=0`, `codeB=1`, `risk=Exception`, `startup=false` | `2097152/RiskDefer` | 异常边风险延迟，未编译；对 silent 微路径有保护作用 |
| `logging_simple` | `logging:LogRecord.__init__` | 4 | 70.6ms | `BranchFSM`, dims=`Control+Object`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 日志输出核心对象构造；延迟到高热度后仍编译，不能禁编 |
| `logging_simple` | `__main__:bench_simple_output` | 4 | 32.0ms | `CallDispatcher`, dims=`Object+Dispatch`, `loop=1`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | benchmark 本体，应放行 |
| `logging_simple` | `logging:Logger.callHandlers` | 4 | 28.4ms | `BranchFSM`, dims=`Control+Object`, `loop=3`, `codeB=2`, `risk=None`, `startup=false` | `1000/LowRoi` | 输出调用链核心函数；LowRoi 延迟合理 |
| `logging_simple` | `logging:Logger.findCaller` | 4 | 27.1ms | `BranchFSM`, dims=`Control+Dispatch`, `loop=2`, `codeB=2`, `risk=Exception`, `startup=false` | `1000/LowRoi` | 带异常边的调用者查找；延迟但热后编译 |
| `logging_simple` | `logging:StreamHandler.emit` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Object`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=false` | `2097152/RiskDefer` | 异常边风险延迟，适合作为输出路径风险保护 |
| `logging_format` | `logging:LogRecord.__init__` | 4 | 64.3ms | `BranchFSM`, dims=`Control+Object`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 与 simple 相同，是日志输出核心成本 |
| `logging_format` | `__main__:bench_formatted_output` | 4 | 32.7ms | `CallDispatcher`, dims=`Object+Dispatch`, `loop=1`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | benchmark 本体，应放行 |
| `logging_format` | `logging:PercentStyle._format` | 4 | 4.5ms | `BranchFSM`, dims=`Compute+Control+Object`, `loop=0`, `codeB=0`, `risk=None`, `startup=false`, `compute=true` | `1000/LowRoi` | 轻量格式化函数，compute 提示存在但当前仍被低 ROI 延迟；是否需要放宽需看非 debug A/B |

### 13.4 策略判断

| 观察 | 结论 |
|---|---|
| `silent` 的本体和 `Logger.debug` 很小，`limit=2` | disabled logging 微路径不应被高成本策略误伤 |
| `simple/format` 中多个输出核心函数是 `startup=false + LowRoi + gate_count≈3996` | `LowRoi` 是热度延迟，不是禁编；热日志路径仍能进入 JIT |
| `StreamHandler.emit/flush`、`Logger.isEnabledFor` 等异常边函数被 `RiskDefer` 延迟 | 风险延迟对异常边丰富的 logging 框架有实际保护作用 |
| logging top 编译耗时里有 `importlib.metadata`、`argparse` 等 driver/setup 噪声 | 调 logging 策略时必须看 benchmark-self，不能只看全局 top 编译耗时 |

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

`pickle_pure_python` 是纯 Python 序列化 workload，主体不是 C 加速模块，而是 `pickle._Pickler` 内部的大量对象图遍历、分支判断、动态分发和 memo 操作。真实 worker gate 证据显示，gate 主体是 `BranchFSM`，核心函数如 `save_tuple`、`save_global`、`_batch_setitems`、`_batch_appends` 都是 `startup=false + codeB=2 + LowRoi`，gate 数接近 3996 后仍会编译。

这组样本说明：纯 Python 序列化不是 import 风暴；它是 steady 对象图处理。`LowRoi` 应被理解为热度过滤，不能把 `pickle._Pickler` 的大分支函数全局拦掉。同时，`save/save_reduce/save_str` 这类更大或带异常风险的函数在本口径下没有热到编译，`RiskDefer` 对这些风险函数起到了保护作用。

### 15.2 摘要

| 用例 | 编译事件 | unique compiled | target unique compiled | gate 事件 | unique gated | gate family top | gate reason top |
|---|---:|---:|---:|---:|---:|---|---|
| `pickle_pure_python` | 678 | 175 | 22 | 88631 | 503 | `BranchFSM:66669`, `Mixed:9619`, `CallDispatcher:7936`, `ReflectionMeta:3462` | `LowRoi:86366`, `RiskDefer:1672`, `None:433`, `StartupInit:160` |

### 15.3 代表函数形状表

| 用例 | 函数 | 编译次数 | 编译耗时 | 形状 | 策略 | 判断 |
|---|---|---:|---:|---|---|---|
| `pickle_pure_python` | `pickle:_Pickler.save_tuple` | 4 | 70.8ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `1000/LowRoi` | 纯 Python pickle 核心路径；延迟到高热度后编译，不能禁编 |
| `pickle_pure_python` | `pickle:_Pickler.save_global` | 4 | 52.4ms | `BranchFSM`, dims=`Control+Dispatch`, `loop=2`, `codeB=2`, `risk=HugeCode`, `startup=false` | `1000/LowRoi` | 全局对象序列化路径；高成本但属于主体工作 |
| `pickle_pure_python` | `pickle:_Pickler._batch_setitems` | 4 | 40.8ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=3`, `codeB=2`, `risk=Exception`, `startup=false` | `1000/LowRoi` | dict 批量写入路径；LowRoi 热度过滤合理 |
| `pickle_pure_python` | `pickle:_Pickler._batch_appends` | 4 | 39.4ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=2`, `codeB=2`, `risk=Exception`, `startup=false` | `1000/LowRoi` | list 批量写入路径；热后编译 |
| `pickle_pure_python` | `pickle:whichmodule` | 4 | 31.9ms | `BranchFSM`, dims=`Control+Dynamic`, `loop=3`, `codeB=2`, `risk=Exception`, `startup=false` | `1000/LowRoi` | 动态模块定位路径；延迟但热后编译 |
| `pickle_pure_python` | `__main__:bench_pickle` | 4 | 24.2ms | `CallDispatcher`, dims=`Dispatch`, `loop=2`, `codeB=2`, `risk=None`, `startup=false` | `2/None` | benchmark 本体驱动函数，应放行 |
| `pickle_pure_python` | `pickle:_Pickler.memoize` | 4 | 13.6ms | `ObjectManipulator`, dims=`Control+Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | memo 操作小函数，放行合理 |
| `pickle_pure_python` | `pickle:_Pickler.put` | 4 | 10.4ms | `BranchFSM`, dims=`Compute+Control+Dynamic`, `loop=0`, `codeB=0`, `risk=None`, `startup=false`, `compute=true` | `1000/LowRoi` | 轻量 compute 混合路径；当前延迟但热后编译 |
| `pickle_pure_python` | `pickle:_Pickler.save_reduce` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=0`, `codeB=3`, `risk=Exception+HugeCode`, `startup=false` | `2097152/RiskDefer` | 高风险大函数，本口径下未热到编译；风险延迟合理 |
| `pickle_pure_python` | `pickle:_Pickler.save` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=0`, `codeB=2`, `risk=Exception+HugeCode`, `startup=false` | `2097152/RiskDefer` | 通用入口风险高；需非 debug A/B 判断是否需要专项放宽 |
| `pickle_pure_python` | `dataclasses:_process_class` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=true` | `2097152/RiskDefer` | driver/setup 导入噪声，风险延迟正确；不应和 pickle 本体混在一起判断 |

### 15.4 策略判断

| 观察 | 结论 |
|---|---|
| `pickle._Pickler` 核心函数多为 `startup=false + BranchFSM + codeB=2` | pure-python pickle 是 steady 对象图遍历，不是 import-window 拦截对象 |
| 多个 `LowRoi` 函数 gate 数接近 3996 且最终编译 | `LowRoi` 在该用例中保留热后收益 |
| `save/save_reduce/save_str` 等大函数未编译，走 `RiskDefer` 或超高 `LowRoi` 阈值 | 风险延迟可减少低热大函数编译成本，但是否影响收益要看非 debug A/B |
| `dataclasses` startup 样本来自 setup/driver | 策略判断要优先看 benchmark-self，不能被全局 top 编译耗时带偏 |

## 16 用例：deepcopy 系列

### 16.1 总体判断

`deepcopy` 一次 benchmark run 覆盖三个子场景：标准对象深拷贝、`__reduce__` 路径和 memo 复用路径。真实 worker gate 证据显示，核心收益路径集中在 `copy._reconstruct`、`copy._deepcopy_dict/list/tuple` 和三个 `__main__:benchmark*` 驱动函数。它们都是 steady 对象图遍历和重建，不属于 startup/import。

这组样本进一步说明：对象图 workload 不能按“非数值 + 控制/对象 + highcost”全局粗拦。`copy._reconstruct` 被 `LowRoi` 延迟到 1000，但 gate 数接近 7992 且最终编译；`copy.deepcopy` 入口本身在本口径下没有编译，说明当前策略会把热点放到更具体的内部函数，而不是盲目编译所有入口。

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
| `deepcopy` | `copy:_deepcopy_tuple` | 8 | 13.2ms | `BranchFSM`, dims=`Control`, `loop=2`, `codeB=1`, `risk=Exception`, `startup=false` | `1000/LowRoi` | tuple 深拷贝路径；异常边导致延迟，但热后编译 |
| `deepcopy` | `__main__:benchmark_memo` | 4 | 12.6ms | `ObjectManipulator`, dims=`Object+Dispatch`, `loop=1`, `codeB=0`, `risk=None`, `startup=false` | `2/None` | memo 子场景驱动函数，应放行 |
| `deepcopy` | `copy:_reconstruct.<locals>.<genexpr>` | 4 | 7.0ms | `BranchFSM`, dims=`Control+Dispatch+Dynamic`, `loop=1`, `codeB=0`, `risk=None`, `suspend=true`, `startup=false` | `2/None` | suspendable 小热路径，不能因为 suspendable 一刀切禁编 |
| `deepcopy` | `copyreg:__newobj__` | 4 | 4.4ms | `Mixed`, dims=`Object+Dispatch`, `loop=0`, `codeB=0`, `risk=None`, `startup=false` | `1000/LowRoi` | reduce 重建辅助函数，低 ROI 延迟后热度足够时编译 |
| `deepcopy` | `copy:deepcopy` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Dispatch`, `loop=0`, `codeB=2`, `risk=None`, `startup=false` | `2097152/LowRoi` | 入口函数未编译，当前策略更偏向内部热点 |
| `deepcopy` | `copy:_keep_alive` | 0 | 0.0ms | `BranchFSM`, dims=`Control`, `loop=0`, `codeB=0`, `risk=Exception`, `startup=false` | `2097152/RiskDefer` | 低热异常边辅助函数，风险延迟合理 |
| `deepcopy` | `dataclasses:_process_class` | 0 | 0.0ms | `BranchFSM`, dims=`Control+Object+Dynamic`, `loop=3`, `codeB=3`, `risk=Exception+HugeCode`, `startup=true` | `2097152/RiskDefer` | dataclass 初始化噪声，import/setup 风险延迟正确 |

### 16.4 策略判断

| 观察 | 结论 |
|---|---|
| `copy._reconstruct` 是 `startup=false + LowRoi` 且最终编译 | deep copy 的对象重建核心路径应允许热后编译 |
| `copy._deepcopy_dict/list` 和三个 `__main__:benchmark*` 驱动函数均为 `2/None` | 小而热的对象图操作应直接放行 |
| `copy.deepcopy` 入口未编译，内部热点编译 | 当前策略不会盲目编译所有入口函数，这对对象图 workload 是有利边界 |
| `copy._reconstruct.<locals>.<genexpr>` 是 suspendable 但放行 | suspendable 只能作为风险信号，不能单独决定禁编 |
| dataclass startup 样本走 `RiskDefer/StartupInit` | import/setup 保护有效，但不应用来否定 steady deepcopy 热路径 |

## 17 当前策略判断汇总

| 规则 | 支撑用例 | 结论 |
|---|---|---|
| `highcost > 0` 包含 `>= 2` | `2to3` | 成立。直接 JIT debug 口径显示 `>0` 相比 `>=2` 进一步减少编译数和编译耗时；pyperformance 墙钟噪声不能用来否定集合包含关系 |
| import window 内高成本非数值形状应延迟 | `2to3`、`coverage` startup=true 样本 | 成立。目标是削减 startup/import compile storm |
| steady-state highcost 不能全局延迟 | `unpack_sequence` | 成立。`do_unpacking` 是 `ObjectManipulator + codeB=3 + risk=HugeCode`，但必须放行 |
| suspendable 不能一刀切禁止 | `generators` | 成立。`Tree.__iter__` 延迟到 1000 后仍可热后编译 |
| `compute=true` 默认放行 | `coverage:fibonacci`、`generators:tree` | 成立。数值/compute 提示是收益信号 |
| coverage 回归不能直接驱动 import-window 策略收窄 | `coverage` | 成立。其高成本函数多在 steady worker，属于单独 ROI 问题 |
| 框架型 steady highcost 需要单独 ROI 判断 | `sqlalchemy_declarative`、`sqlalchemy_imperative` | 成立。ORM/engine 热路径里存在 `BranchFSM`、`ObjectManipulator`、`ReflectionMeta` 大函数，不能由 import-window 规则直接拦 |
| parser/optimizer 树处理不能按 `ObjectManipulator + codeB=2` 全局延迟 | `sqlglot_v2` 系列 | 成立。`Parser._parse`、`pushdown_projections`、`optimize_joins` 等是 benchmark 主体工作 |
| `StartupInit` 是阶段条件，不是单独拦截理由 | `sqlalchemy`、`sqlglot` startup 样本 | 成立。只有 startup/import 内高成本、非数值、非 compute 形状才是目标；小函数和 compute 函数不应被误伤 |
| `LowRoi` 是热度延迟，不是禁编 | `logging_simple`、`logging_format`、`sympy_expand`、`sympy_str` | 成立。日志输出和符号计算核心函数会在 gate 数足够高后编译 |
| highcost 符号计算不能全局延迟 | `sympy` 系列 | 成立。`Mul.flatten`、`Expr.expand`、`simplify`、`Printer._print` 等是主体工作，不是 startup/import 噪声 |
| benchmark-self 优先于全局 top 编译耗时 | `logging` 系列 | 成立。全局 top 可能混入 `importlib.metadata`、`argparse`、driver/setup 函数，策略判断必须回到本体函数 |
| pure-python 对象图序列化不能全局拦 `BranchFSM + codeB=2` | `pickle_pure_python` | 成立。`save_tuple/save_global/_batch_*` 是主体路径，LowRoi 延迟后仍编译 |
| 对象图重建热点应允许热后编译 | `deepcopy` | 成立。`copy._reconstruct` 延迟到 1000 后仍进入 JIT，内部热点优先于入口函数 |
| `RiskDefer` 对低热大函数有效，但不能替代收益判断 | `pickle_pure_python`、`deepcopy` | 成立。部分入口/通用函数未编译是保护，但上线前仍需非 debug A/B 确认收益是否丢失 |

## 18 待补清单

| 优先级 | 项 | 目的 |
|---|---|---|
| P0 | 生成当前 `highcost > 0` 口径下 `2to3` 的完整 shape TSV | 让 `2to3` 主目标与当前策略完全对齐 |
| P0 | 将 `2to3` 拆成 `import lib2to3.main` 与 `main() refactor` 两阶段函数表 | 区分 import-window 应延迟与 steady/refactor 应放行 |
| P1 | 给 `coverage` 补非 debug 性能 A/B 与候选级动态收益 | 判断是否需要 steady coverage 专项策略，而不是污染 import-window 策略 |
| P1 | 给 `sqlalchemy` 系列补非 debug A/B 与候选级动态收益 | 判断 ORM/engine steady highcost 函数是否需要专项阈值，而不是扩大 import-window 策略 |
| P1 | 给 `sqlglot_v2` 四个子用例分别补非 debug A/B | 区分 parse/transpile/optimize 的真实收益和误伤边界 |
| P1 | 给 `logging` 三个子用例分别补非 debug A/B | 区分 silent 微路径、simple 输出路径、format 格式化路径是否需要差异化阈值 |
| P1 | 给 `sympy` 四个子用例分别补非 debug A/B | 判断符号计算 highcost 函数的动态收益是否覆盖编译成本 |
| P1 | 给 `pickle_pure_python` 补非 debug A/B | 判断纯 Python 序列化大分支函数热后编译收益是否覆盖编译成本 |
| P1 | 将 `deepcopy` 拆成 `deepcopy/deepcopy_reduce/deepcopy_memo` 三个子场景分别取证 | 当前 run 聚合了三类子场景，后续需要拆分细看对象重建、reduce、memo 的差异 |
| P1 | 给 `dask`、`dulwich_log`、`bench_mp_pool` 补同格式表 | 扩大非 JIT 用例样本，避免只围绕 `2to3` 调参 |
| P2 | 给 `scimark_fft/scimark_lu/scimark_sor/scimark_monte_carlo` 补同格式表 | 验证 JIT 用例误伤边界 |
