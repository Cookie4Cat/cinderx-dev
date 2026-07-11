# M10 第二十七轮：中间带体检——28 个未专项关注用例的分诊

日期：2026-07-10　分支：`dryrun/m10-middle28-triage`　基线：!74。
背景：注意力审计显示 0.95-1.05 段 47 项中 28 项从未专项验尸。本轮
为诊断轮（不实施优化），产出构成分类与阻塞短名单。

## 一、方法

体检双件套（tools/diag_shim.py + tools/diag_orch.py）：伪 Runner
截获各 bench 基准函数 → 进程内自适应暖机 → 写就绪标记供 perf
attach → 10s 稳态窗 → 倾倒 JIT 统计（编译数/码量/deopt 面）。
稳态符号份额按 DSO 级聚合（首版 symbol 级 --percent-limit 0.3 把
散布于数百小函数的 JIT 码样本切没了——**平坦热面的份额统计必须
DSO 级聚合**，教训入档）。解释热点点名用 gdbwho2 断点采样
（Ci_EvalFrameDefault_311 入口取 co_qualname）。

## 二、分类结果（28 项）

**C 天花板（9 项，~1.0 即物理上限，不亏）**：pidigits（py-C 96%）、
regex_dna（99%）、sqlite_synth（sqlite 82%）、xml_etree（C 扩展
48%+py-C 50%）、json_dumps、meteor_contest（py-C 86%）、
networkx_connected_components、mdp（py-C 88%）、
typing_runtime_protocols（py-C 79%）。

**已兑现/事件循环栈（10 项）**：async_tree 全族 8 项（asyncio C
机器 75-80%，解释残留仅 2-3.6%，协程轮成果已兑现，天花板近）、
tornado_http（jit 14.3%，网络栈 other 25%）、logging（jit 16.3%）。

**结构性（1 项）**：coverage 0.959——trace 钩子使全部代码回退解释
（interp 27.8%+cinderx 钩子 37.8%），与"可观测性=回退解释器"的
既有产品判决一致，不修。

**阻塞嫌疑（6 项，解释残留 5-40%）**：nqueens 40.3%、sqlglot_v2
15.8%、bpe_tokeniser 14.3%、sqlglot_v2_optimize 11.3%、
networkx_k_core 9.7%、sympy 9.4%（另 mdp 5.0 归 C 天花板）。

## 三、阻塞组归因

1. **nqueens = 生成器策略税的具象**：稳态 JIT 码占比字面 0.0%——
   主体为 yield 置换生成器，同步生成器不自动编译策略（!51）使其
   全程解释。旗标 A/B（PYTHONJITCOMPILESYNCGENERATORS=1）仅
   +3.7%——恢复仪式吞掉编译收益，与 !51 判决一致。**升级路径 =
   生成器恢复仪式根治（M6 级），非翻策略**。sympy 的残留主体同族
   （genexpr 断点采样 321/600，FactKB.deduce_all_facts 生成器
   表达式）。
2. **bpe_tokeniser = 新鲜函数对象接不上既有编译入口（产物级缺陷，
   本轮最大发现）**。断点采样 591/600 集中于
   `bpe_train.<locals>.<lambda>`——训练循环每轮新建
   `max(stats, key=lambda x: stats[x])`。最小复现三判据：
   - 该 lambda 的 code 曾被编译（某实例跨过阈值，
     get_compiled_functions 可见）；
   - **每轮新建的实例 is_jit_compiled == False**——新函数对象
     永远走解释；
   - 对照组同一实例复用则正常编译。
   即：code 级热度与编译产物均在，但新函数对象的入口未挂接既有
   编译入口。受害面为一切"热路径新建短命闭包/lambda"惯用形
   （排序 key/回调/装饰器包装），疑同为 sympy 包装器族与 sqlglot
   残留的成因。修复方向：入口桩查得 code 级既有编译入口时就地
   挂接本函数对象（lookupCompiledForCall 命中即换入口）。
3. **sqlglot_v2 15.8%**：gdb 断点采样因暖机 bp 开销超时未点名；
   据形态（解析器 = 生成器 + 新建 lambda 密集）推定为 1+2 复合，
   待修复 2 后复测定谳。

## 四、量化汇总（稳态 DSO 份额，%）

| bench | ratio | jit 码 | py-C | cinderx | 其它 | interp |
|---|---|---|---|---|---|---|
| nqueens | 1.021 | 0.0 | 56.4 | 42.6 | 1.0 | 40.3 |
| coverage | 0.959 | 0.8 | 51.3 | 37.8 | 10.1 | 27.8 |
| sqlglot_v2 | 1.022 | 10.0 | 65.4 | 21.5 | 3.1 | 15.8 |
| bpe_tokeniser | 1.002 | 2.9 | 78.5 | 16.6 | 2.0 | 14.3 |
| sqlglot_v2_optimize | 1.000 | 13.4 | 66.6 | 18.0 | 2.0 | 11.3 |
| sympy | 0.983 | 9.1 | 70.1 | 17.8 | 3.0 | 9.4 |
| networkx_k_core | 1.023 | 1.6 | 77.7 | 11.9 | 8.8 | 9.7 |
| async_tree 族(均) | ~1.01 | 5-8 | 74-80 | 7-10 | 7-10 | 2-3.6 |
| C 天花板组(均) | ~1.0 | 0-6 | 50-99 | 0-9 | — | 0-0.6 |

pathlib 垫片不兼容（iterdir 需工作目录布景）未采，待手工补。

## 五、结论与下一步

- 28 项中 20 项为 C 天花板/已兑现/结构性——"被忽略"实为"无债可收"；
- 真实阻塞 6 项归两个根：**生成器/恢复仪式**（既判，根治属 M6）与
  **新鲜函数对象挂接断链**（新缺陷，修复面小、受益面广，建议下轮
  首发）；
- 修复挂接断链后复测 bpe/sqlglot/sympy 三项定谳残余。
