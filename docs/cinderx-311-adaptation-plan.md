# CinderX 适配 CPython 3.11 技术设计书

> 状态：开发指导文档（随里程碑更新）｜ 更新：2026-07-02 ｜ M0 已基本完成
>
> 本文档是 3.11 适配工作的施工图：定位与决策、测试门禁体系、代码隔离策略、里程碑与验收标准。后续开发以本文档为准，变更需更新本文档。

---

## 1. 定位与目标

**3.11 是产品线；3.14 是参考线。** 实际生产客户以 CPython 3.11 为主（保守版本策略），3.14 暂无部署场景。因此：

- **产品目标**：把 CinderX JIT 适配到 **openEuler 24.03-LTS-SP3 的 python3（3.11.6 + 发行版补丁）**，达到可交付客户、可长期维护的状态。release-blocking 门禁挂在 3.11 线上。
- **3.14 的三重职能**（必须持续维护的理由）：
  1. **上游改进的引入通道**：Meta 的共享 JIT 核心改进（HIR/LIR/codegen）通过 3.14/3.15 sync 流入，直接惠及 3.11 产品——前提是共享代码保持可 sync；
  2. **oracle 与参考实现**：M9 性能标尺（3.14-cinderx 加速比）、差分语料自检（diffgate_314）、行为参考答案；
  3. **技术验证环境**：LWF、inliner 等先在 3.14 上验证，再在 3.11 上落地。
- **能力沉淀视角**：客户保守意味着产品面长期是"尾随版本"（今天 3.11，未来可能 3.12）。本次建设的构建管线、适配层、差分门禁、opcode 覆盖矩阵都是**下次版本移植的可复用基础设施**，取舍时禁止"一次性方案省两周但机制不可复用"。

## 2. 范围

**保留**：普通 Python 函数 JIT；3.11 解释器循环适配；frame 基础模型（轻量帧开关控制、默认关闭）；CALL / LOAD_METHOD / vectorcall 路径接管；deopt 回解释器；属性/方法/全局变量内联缓存（版本号守卫）；同步 generator JIT；交付工程（openEuler 目标）。

**不做**：Static Python；自定义 Cinder bytecode；Parallel GC；Lazy Import；Free Threading；**通用 PyPI cp311 wheel**（交付物是面向 openEuler 的内部分发，见 M10）；完整 3.14 parity；协程与异步 generator 的 JIT（安全拒编回退）；调用点内联与轻量帧默认开启（v1.1）。

## 3. 已拍板决策

**P0（总原则）— 分级差分契约**：差分测试不追求完全等价，分三级：

- **硬等价类**（永不允许偏差）：返回值、控制流结果、异常类型与消息逐字符、traceback 行列号、用户可见 hook（property/descriptor/`__getattr__`/`__eq__` 等）触发次数、缓存失效可见性。
- **允许偏差类**（版本化白名单 `ci_pipeline/diffgate/allowlist.toml`，每条带 rationale + 钉住测试）：析构与 GC 时机、`id()`、`getrefcount` 绝对值、frame 对象身份、tracing 激活时栈上 JIT 帧无事件等。
- **中间类**（weakref 顺序、`__del__` 再入、deopt 瞬间 locals 等）：遇首例逐条裁决入册，禁止口头豁免。

| # | 决策 | 内容 |
|---|------|------|
| D1 | 合入锚点 | 自有 fork 长期维护，不合上游。上游 sync 时 3.11 + 3.14 双跑 CI |
| D2 | 穿刺处置 | 推倒重建。前提三件套：测试资产先固化（已完成，见 §8）、门禁先对穿刺跑通自证（已完成）、最小端到端链路设强制检查点（M4 出口④） |
| D3 | 解释器策略 | 逐字复制 ceval.c + 独立补丁文件（每条带理由）+ 构建期源码哈希校验。不手写精简解释器 |
| D4 | Frame 模型 | materialized frame 默认，LWF 开关控制默认关；翻转为 v1.1 独立里程碑；v1 性能目标按 LWF-off 校准 |
| D5 | 缓存失效 | 版本号守卫拉式验证（3.11 原生模式）。值缓存用 `ma_version_tag`，仅结构性事实可用 `dk_version`；global cache 从 watcher 推式间接槽改造为拉式 |
| D6 | Generator 范围 | 仅 `CO_GENERATOR` 且无 async 标志可编译；协程/异步 generator 白名单拒编；async 类基准预期 ≈1.0x 写入 M9 目标表 |
| D7 | 版本锚点 | **openEuler 24.03-LTS-SP3 python3 包（3.11.6 + 约 50 个发行版补丁，全部客户环境统一按此锚定）**。M1 机器验证：SRPM 补丁应用后与上游 3.11.6 在 JIT 相关核心文件集（约 15 个）diff 为空则按上游 vendor，否则按 openEuler 树 vendor。源码哈希门禁只锁核心文件集（stdlib CVE errata 不误报）。JIT 不许"顺手修对"上游后续版本才修的 bug |
| D8 | Tracing | 对齐 cinderx 3.14 pause 语义：patch settrace/setprofile → 停止编译 + 已编译函数入口换回解释器；栈上 JIT 帧跑完为止（入白名单）；清除后 reopt 恢复 |
| D9 | 缓存引用 | borrowed ref + guard 通过前不解引用；debug 构建 guard 失败路径写 poison；ASAN 门禁覆盖缓存测试 |
| D10 | 差分对象 | 三级对照链（见 §4.1）：stock ↔ JIT-off 零容忍等价；JIT-off ↔ JIT-on 固化基线管理。绝不与 3.14 输出做正确性差分 |
| D11 | 正确性语料 | 四层语料（§4.4）；pyperformance 从正确性语料除名，仅以性能身份出现在 M9；"高温类"缺口由 diffgate 内 3–5 个手写热循环 case 覆盖 |
| D12 | 共享代码隔离 | 按 delta 尺寸分流（§5），延续既有 `#if PY_VERSION_HEX` 约定于小差异，结构性差异走版本目录，重复模式晋升 `py-portability.h`，存量 229 处按棘轮收编 |

**待拍板（不阻塞开工）**：① `PyEval_SetTrace` C 层旁路——推荐 JIT prologue 加 `use_tracing` 检查，否则入册豁免；② 偏差白名单与失败基线的批准权归属（单一 owner）；③ 中间类偏差首例裁决流程演练。

## 4. 测试门禁体系

### 4.1 差分对象：三级对照链（D10）

差分对象是**同一个 3.11 解释器二进制、同一容器环境下的自己**，唯一变量是 JIT 维度：

```
① stock 模式          ② JIT-off 模式            ③ JIT-on / JIT-deopt 模式
（不加载 cinderx）     （加载 cinderx，JIT 关）    （强制编译 / 强制反优化）
        └── 逐字相等，零容忍 ──┘└── 差分 + 固化基线燃尽 ──┘
```

①↔② 验证"加载 cinderx 本身"（vendored 解释器循环、import 副作用），M2 出口，不允许基线；②↔③ 验证 JIT，前期大面积红，走基线燃尽。分两段的价值是**归因**：红了立刻知道是解释器循环坏了还是 JIT 编错了。

### 4.2 Suite 结构（与 3.14 对齐 + 3.11 独有第四条腿）

```
3.14: runtime.toml / cinderx_local.toml / libtest(env) / diffgate_314(daily, 语料自检)
3.11: runtime_311.toml / cinderx_local_311.toml / libtest_311.toml / diffgate_311.toml
pipeline: pr-311   = diffgate_311 + runtime_311 + cinderx_local_311（分钟级）
          daily-311 = pr-311 + libtest_311 + ASAN + refleak + 热循环/性能追踪
```

diffgate_314 的职能是验证**语料和 harness 自身**：347 case 在成熟 3.14 上应接近全绿，双红 case 要么语料依赖了未定义行为、要么是 3.14 真 bug。

### 4.3 三类既有测试套件（RuntimeTests / test_cinderx / Lib test）的 3.11 方案

**RuntimeTests**：gtest filter 白名单分三层点亮，白名单文件进版本库。判定语义"期望绿"（自家测试，红即修）：
- M1 起：版本无关基础设施层（bitvector/intrusive_list/copy_graph/dataflow/block_canonicalizer/branch_relaxation/code_allocator/elf/LIR 系）；
- M4 起：前端层（`hir_tests/*.txt` 需 3.11 快照段，随 opcode 家族落地逐个补；bytecode_test/bytecode_offsets/hir_frame_state）；
- M3/M6 起：frame/deopt 层（deopt_test/deopt_patcher/gen_asm/inline_cache/ReifyFrameTest）。

**test_cinderx**（走现有 wheel→venv→runner 流程）：
- 必跑：test_jit_disable / test_jitlist / test_jit_count_calls / test_frame_evaluator / test_oss_quick（M2/M4）、test_jit_frame（M3）、test_jit_exception（M6）、test_jit_attr_cache / test_jit_global_cache（M7）、test_jit_generators（M8）、test_jit_specialization（M4，PEP 659 交互）、test_jit_support_instrumentation（D8）、test_cinderjit（版本 skip 裁剪）；
- **改写后跑**：test_jit_coroutines / test_jit_async_generators——断言目标改为"正确拒编且回退结果正确"（D6），不许整体 skip；
- 排除：test___static__ / test_compiler* / test_parallel_gc* / test_subinterpreters / test_asynclazyvalue / test_python312+314_bytecodes（M4 交付 test_python311_bytecodes）/ perf 系（M9 再启）。

**libtest**（复用 lib_test_runner：dispatcher 多 worker、崩溃隔离、JSON 汇总）：
- **配置 1 stock 记录**：不加载 cinderx 跑全量，JSON 落盘为 oracle（环境性失败被自然吸收，无需 skip 清单）；
- **配置 2 JIT-off 等价**（M2 出口）：**全量**，逐 test 与配置 1 比对，diff 必须为空；
- **配置 3 JIT-on 差分**（M4 爬坡、M6 进 daily）：`--adaptive-compile-after` 跑**两档阈值（2 和 24）**覆盖特化前/后编译的字节码；**裁剪子集**（约 35 模块，选择标准是"测解释器而非测库"）：
  - 语言核心：test_grammar/builtin/types/descr/descrtut/dict/set/list/tuple/int/float/str/exceptions/raise/traceback/call/keywordonlyarg/unpack/unpack_ex/scope/super/property/metaclass/augassign/with/contextlib
  - generator/协程：test_generators/genexps/pep380/coroutines（拒编回退正确性）
  - 已识别风险区：test_sys/sys_settrace/sys_setprofile（D8 的官方验证集）/gc/weakref/signal（eval breaker）/inspect/frame
  - 排除首批：网络系、进程系、test_asyncio（M8 后取切片）、GUI/平台系、纯 C 扩展库系、test_io（daily）
  - 清单文件进版本库，随里程碑扩张（M3 加 pdb/bdb，M7 加 pickle/copy）。

### 4.4 正确性语料四层（D11）

| 层 | 内容 | 完备性刻度 | 状态 |
|---|---|---|---|
| 1 手写风险定向 | unbound 31 / ic_mutation 15 / frames 16 + 热循环 3–5 个（千万次迭代 + 校验和） | 回归沉淀纪律：**任何渠道发现的 bug 必须蒸馏成 case 才能销案** | 62 已建，热循环待补 |
| 2 枚举生成 | calls 285（15 被调对象 × 19 调用点形态，exec 生成独立字节码） | 维度表 review | 已建 |
| 3 opcode 定向（M4） | 每个 3.11 opcode 一组最小程序 | **覆盖矩阵三态报表（已编译/已拒绝/已测 deopt）的空格即缺口清单** | M4 交付 |
| 4 Lib/test 全量 | §4.3 配置 2/3 | CPython 测试套自身 | M2 起 |

语料准入纪律：确定性（HASHSEED=0、禁随机/时间/网络）；自恢复（全局变异 finally 还原）；输出即断言（case 不写期望值，oracle 输出就是期望）；一案一行为。

### 4.5 门禁演化表（能力 × 里程碑）

| 阶段 | 新增门禁能力 | 层 |
|---|---|---|
| M0 | diffgate（三模式+崩溃恢复+基线）、基础 libtest 差分、白名单+钉住、deopt v0（uncompile 级）、热循环 case | PR |
| M1 | opcode 表 verifier（构建期）、源码哈希、双版本 CI、三类测试套件的 311 suite 骨架、stock oracle 首录 | 构建期+PR |
| M2 | libtest JIT-off 全量等价、refleak 抽样（regrtest -R）、**ASAN 门禁（脚本 + 首跑，对重建产物；须在 M6 开工前可用）** | daily |
| M3 | frame reify 冒烟并入语料 | PR |
| M4 | opcode 覆盖矩阵、拒编安全阀测试、最小端到端链路常驻 CI | PR |
| M5 | 调用矩阵加引用计数断言 | PR |
| M6 | deopt v0→v1（任意 guard 点回退+状态逐项等价）、异常注入 fuzz（count-then-inject）、libtest JIT-on 进 daily、settrace 往返 | PR+daily |
| M7 | 变异套件加再入用例、ASAN 对 IC 路径 PR 必跑 | PR |
| M8 | gen 挂起点 deopt ASAN 专项 | PR |
| M9 | ratio 追踪（vs 3.14-cinderx ratio）、helper heatmap、夜间性能回归 | daily |

治理三规则：**基线只准收缩**（新增失败需 owner 批准，基线尺寸曲线 = 项目健康度）；**PR 层保持分钟级**（慢门禁归 daily）；**门禁上岗前必须证明抓到过真 bug**。

### 4.6 参考实现：V8 Foozzie（correctness fuzzing）的可借鉴设计

V8 的差分正确性体系（`tools/clusterfuzz/foozzie/`）与本方案内核同构：基线配置的输出即期望值，无人写断言；其 baseline 固定为**最不优化的配置**（纯解释器 ignition，而非 default）——oracle 越简单越可信，与我们以"不加载 cinderx 的解释器"为 oracle 是同一哲学。经代码分析确认的可借鉴点及挂靠：

| 借鉴点 | 挂靠 | 说明 |
|---|---|---|
| 豁免条目绑 issue 编号 | 立即 | V8 的源码/输出级豁免每条必须映射 bug；allowlist.toml 条目在 rationale 外增加 issue 字段，中间类偏差裁决后有据可查 |
| 豁免生命周期管理 | 常驻 | V8 只在出现差异时评估豁免，并统计"不再命中的豁免"以便删除——防止死豁免堆积 |
| 差分报告按根因聚类 | M2 前 | 按失败特征（异常类型+消息模式）聚类，把"75 个失败"呈现为"6 个 bug 家族"；libtest 26/26 同根因的场景已证明必要 |
| 配置矩阵隔离子系统 | M7 起 | V8 用 no_ic/slow_path/jitless 等配置对，让每一对差异自带归因（关 IC 对比有 IC → 差异必是缓存 bug）。cinderx 有现成开关（disable_specialized_opcodes 等），M7 增加"关缓存 vs 开缓存"差分轴 |
| 跨架构差分 | M9/常驻 | 同一语料在 x86_64 与 aarch64 上对跑，输出应逐字相同——抓架构特定 codegen bug；V8 另有 fallback 机制（跨架构差异先在基准架构复现，能复现则归为通用 bug） |
| 不确定性 mock 注入 | M6+（引入 fuzz 语料时） | V8 在每个 case 前注入 mock（Math.random/Date 换确定序列），从源头掐死不确定性而非在比对端豁免；固定语料阶段靠语料纪律即可 |
| harness 自身测试 ≥ harness | 持续 | V8 的 harness 测试代码量超过 harness 本体（694:625 行），且内建冒烟机制；印证"看住看门人"的投入正当性，钉住测试随白名单同步加厚 |

**明确不借鉴**：Foozzie 的集群化部分（3 秒超时、海量随机执行、失败去重、自动最小化）服务于 fuzz 场景；本方案是 CI 门禁场景（固定语料、基线燃尽、分钟级反馈），抄差分思想与豁免治理，不抄集群机器。生成式 fuzz 语料（V8 js_fuzzer 的变异机制）作为第四语料机制的参照，M6 后再评估。

### 4.7 参考实现：JSC（JavaScriptCore）的故障注入体系

JSC 走的是与 V8 互补的路线：**断言测试套 × 配置矩阵 + 确定性故障注入**（`Tools/Scripts/run-jsc-stress-tests` 63 种运行变体；`OptionsList.h` 内建注入开关）。三类机制经源码确认，均为"先跑一遍计数、再按序号注入"的确定性模式（count-then-inject），可精确复现：

| JSC 机制 | 原理 | 对本方案的移植 | 挂靠 |
|---|---|---|---|
| **Exception fuzz**（`--useExceptionFuzz` + `fireExceptionFuzzAt=N`，js-exception-fuzz 驱动） | 首跑统计潜在异常检查点总数，随后按随机序号在第 N 个检查点注入合成异常，验证异常正确传播/捕获 | **对 ⓪ 号 bug 类（JIT 异常路径损坏）直接对症的系统性测法**：cinderx 在异常检查点（helper 返回 NULL 处）加同款注入开关，语料全量扫描异常传播正确性 | M6（与 forced-deopt harness 同期，共享 count-then-inject 基建） |
| **OSR exit fuzz**（`--useOSRExitFuzz` + `fireOSRExitFuzzAt/AtOrAfter`） | 同模式：在第 N 个 deopt 出口强制触发回退 | 即本方案 forced-deopt v1（M6 出口①）的成熟先例；旋钮语义照抄：at-N / at-or-after-N 两档 | M6 |
| **Executable allocation fuzz**（`fireExecutableAllocationFuzzAt=N`） | 在第 N 次可执行内存分配时注入失败，验证优雅回退解释器 | cinderx 对应：代码缓存分配失败/超限（set_max_code_size 已有）时的回退正确性专项 | M8 后（健壮性轴） |

另有三条设计原则值得吸收：① **可测性旋钮内建于运行时而非测试脚本**（所有注入开关是 VM 一等选项，harness 只是薄驱动）——cinderx 的注入开关应实现在 C++ 运行时层，暴露为环境变量/cinderjit API；② **`--scribbleFreeCells`**（释放单元写毒值）作为常开的 VM 选项与 D9 的 debug poison 同思路，可在 debug 构建默认开启；③ **eager 变体**（`EAGER_OPTIONS` 把全部编译阈值压到 10–20）等价于我们的低阈值 libtest 档位，印证双阈值设计。

### 4.8 参考实现分析结论（V8 / JSC / 本方案对照）

两家工业级 JIT 的正确性体系走了互补路线，本方案的各组成部分均可对应到其中一方的成熟实践：

| | oracle 形态 | 语料来源 | 突出强项 |
|---|---|---|---|
| V8 Foozzie | 配置对之间的输出差分（无需人写断言） | 生成/变异（js_fuzzer） | 跨架构差分、豁免治理（绑 bug、生命周期管理）、baseline 取最不优化配置 |
| JSC | 断言测试套 × 配置矩阵 | 手写 stress 套件 | 确定性故障注入（异常/deopt 出口/内存分配，count-then-inject 模式）、可测性旋钮内建于运行时 |
| 本方案 | 兼取两者：diffgate 为输出差分（V8 式），libtest 多配置为断言套 × 矩阵（JSC 式） | 四层语料（§4.4） | 固化基线燃尽管理（版本移植场景特有的需求，两家均无） |

主要结论：
1. **本方案没有孤立发明的部分**——输出差分、配置矩阵、故障注入、豁免治理四个支柱各有经工业验证的参照系；
2. **oracle 取最简配置**（V8 用纯解释器 ignition，本方案用不加载 cinderx 的解释器）是两家共同印证的根基性选择；
3. **故障注入必须实现于运行时层**（JSC 全部注入开关为 VM 一等选项，测试脚本仅为薄驱动）——M6 的 forced-deopt 与异常注入开关应做进 cinderx C++ 运行时，暴露为环境变量/cinderjit API；
4. **异常注入 fuzz（JSC exception-fuzz 同款）与已知问题 ⓪ 直接对症**，已纳入 M6 主要内容与出口条件⑤；
5. 基线燃尽管理是本方案因"前期大面积不等价"的移植场景而独有的机制，两家参照系中无对应物，需自行维护其治理纪律（§4.5 三规则）。

## 5. 共享代码隔离策略（D12）

现状：共享代码已有 229 处 `PY_VERSION_HEX`（44 文件；builder.cpp 30、frame.cpp 21），`Common/py-portability.h` 已是适配层雏形。3.11 差异的特殊性：大 delta（整段平行控制流）占比远高于既有 3.12/3.14/3.15 同代差异。

**按差异形态分流**：

| 差异形态 | 机制 | 例子 |
|---|---|---|
| 小 delta（≤5 行，取值/字段级） | 延续内联 `#if`（既有约定） | 字段名、accessor 微调 |
| 同一 `#if` 模式重复 ≥3 处 | 晋升 `py-portability.h` 原语 | dict 版本号、frame lasti |
| 大 delta（整段平行控制流） | 版本文件/目录派发 | 解释器循环、CALL 协议 HIR 构建、watcher→version-guard 层 |
| 可机械推导 | 生成器 | opcode 表、borrow、头文件 |
| 冷路径一次性差异 | 任意（禁热路径运行时派发） | init/teardown |

**升级信号**（reviewer 用）：一个函数里 ≥3 个 3.11 块、或 3.11 块内嵌套 `#if` → 拆版本文件。
**存量棘轮**：不做一次性迁移；3.11 工作碰到的函数顺手收编其存量 `#if`（builder.cpp/frame.cpp 在 M4/M3 自然消化）；lint 软规则只拦超长 `#if` 块，基线文件豁免存量。

## 6. 3.14 参考线保护

- **字节等同快速通道**：只碰版本目录的 PR，CI 比对 3.14 产物哈希——等同成立则正确性和性能同时被证明未变（日常 90% 的 3.11 PR 走此通道，秒级）。
- **按路径分级触发**：碰共享代码 → 自动升级为 3.14 全量门禁 + 3.11 门禁双跑；上游 sync PR 无条件双跑全量；共享目录 CODEOWNERS 强制 3.14 线 owner review。
- **共享热路径纪律**：3.11 加入的任何钩子必须是 3.14 构建下编译期零开销（验收即字节等同）。
- **夜间 perf A/B**：3.11 与 3.14 各一份（3.11 是产品线性能，3.14 是参考系不漂移）；回归阈值 >1% 且连续两晚复现；报警接 commit-sweep 自动二分（perf-hunt 现有工具链）。
- **热文件清单**：codegen/regalloc/HIR pass 等文件被碰时 CI 提示附带 perf A/B。

## 7. 里程碑

人月口径：1 人月 = 一个人带 agent 工作一个月的端到端产出（设计文档由 agent 起草、人定稿，工时已含）。Owner：A=解释器/Frame 线，B=前端/调用线，C=门禁/基建线。

| # | 标题 | 目标（一句话） | 依赖/并行 | 人月 | Owner | 状态 |
|---|------|----------------|-----------|------|-------|------|
| M0 | 测试与门禁基线 | 门禁先于重建存在；穿刺踩坑全部转为测试资产 | 无；与 M1 并行 | 0.5 | C | **基本完成**（§8） |
| M1 | 3.11 构建与代码隔离 | 3.11 可构建成 wheel 且可被三类测试套件加载运行；3.14 零污染 | 无；与 M0 并行 | 0.5 | A | 未开始 |
| M2 | 3.11 解释器循环适配 | JIT 关闭时与 openEuler 3.11.6 全量测试套逐项等价 | M1 | 3.5 | A | |
| M3 | Frame 基础模型 | frame 链接/还原语义正确，traceback/调试接口可用 | M2 | 2.5 | A | |
| M4 | 3.11 字节码前端适配 | 翻译全覆盖 + 拒编安全阀；完成时最小端到端链路常绿 | 可提前并行，M2 后合入 | 2.0 | B | |
| M5 | 函数调用路径接管 | 全调用形态 JIT 正确（含引用计数断言） | M4 | 1.0 | B | |
| M6 | Deopt 与异常正确性 | 任意点回退状态正确；异常注入全检查点扫荡通过；traceback 列号级等价；tracing pause | M3+M4 | 3.0 | A | |
| M7 | 内联缓存与失效正确性 | 四类 IC 全变异矩阵无 stale/UAF | M5 | 2.0 | B | |
| M8 | Generator 基础支持 | sync gen 可 JIT；协程/async gen 安全拒编 | M6 | 1.0 | A | |
| M9 | 性能分析与优化收敛 | 归因工具 + 按数据收敛热点，限时收口 | 全部 | 1.5 | B+C | |
| **M10** | **交付与支持工程** | **openEuler 交付物（rpm/内部 wheel）、安装文档、支持矩阵声明、问题上报通道** | M9（可与 M9 并行启动） | **1.5** | A+C | 本轮新增 |
| 常驻 | 工程维护（上游同步双跑、门禁加厚、评审、errata 监控） | 双版本持续健康 | 全程 | 2.0 | A+C | |
| | **合计** | | | **约 21–22** | | |

**日历**：3 人约 **8–9 个月**；2 人约 **11–13 个月**（review 带宽是短板，agent 并行度设 WIP 上限）。
**关键路径**：M1→M2→M3→M6→M8；M4 提前并行是唯一日历压缩手段。
**阶段口径**：M0–M2 基础设施，M4 末端完成首次最小端到端验证，M3–M8 正确性主体，M9–M10 性能与交付收口。

### 里程碑详情（主要内容 / 非目标 / 出口条件）

| # | 主要内容 | 非目标 | 出口条件（机器可判） |
|---|----------|--------|----------------------|
| M0 | 差分框架三模式+崩溃恢复+基线机制；白名单+钉住测试；穿刺语料 347 case + 75 失败基线；热循环 case；ASAN 脚本首跑；diffgate_311.toml 接入 run_gate | 不修穿刺 bug；不做 guard 点级 deopt（M6）；不跑 libtest（M2） | ① 差分报告自动产出且穿刺基线固化（diffgate 75/347 + libtest 26/26）✅；② diffgate 语料（含热循环）强制 deopt 下通过（红项入基线）；③ 白名单 v1 入库带钉住测试 ✅（剩余项：热循环 case；ASAN 首跑已移至 M2） |
| M1 | setup.py/CMake 3.11 门控（穿刺 build_ext 原型工程化为 wheel 流程）；opcode 头生成；borrowed-3.11 模板；Interpreter/3.11 骨架；SRPM 核心文件集 diff 验证 + 分流；哈希门禁；compat matrix 翻转；三类测试套件的 311 suite 骨架 + stock oracle 首录；双版本 CI + 字节等同检查 + `#if` lint | 不写 vendored ceval（M2）；不写 JIT/HIR 逻辑；不锁全树哈希 | ① wheel 构建/安装/导入通过；② runtime_311 后端白名单全绿；③ 3.14 产物逐字节不变 CI 证明；④ 哈希不匹配构建失败（注入验证）；⑤ 核心文件集 diff 报告落盘且分流已执行；⑥ borrow 生成跑通 |
| M2 | 逐字 vendor ceval.c + 补丁文件；构建配置对齐清单（dtrace/computed-gotos/编译器选项与发行版一致）；保留 quickening；JIT 插桩点预留；`_CiOpcode_*` verifier 接入 | 不手写精简 loop；不改 JIT 侧 | ① libtest 配置 2 全量与 stock 逐项等价（含 refleak 抽样）；② verifier 常绿；③ 差分链②模式接入 diffgate |
| M3 | frame 抽象层；JIT frame 链接/解链/还原；LWF flag 骨架默认关 | 不翻 LWF 默认；不调 LWF 性能 | reify 冒烟全绿：getframe、跨界 f_back、traceback 行列逐字符、locals 快照、gc 可见性 |
| M4 | PRECALL/CALL/KW_NAMES、LOAD_METHOD、BINARY_OP、FOR_ITER、异常表、特化缓存读取（经 `_PyOpcode_Deopt` 还原）、unbound 守卫；白名单拒编阀；test_python311_bytecodes | 白名单外不"尽力编译" | ① 未知字节码拒编有测试；② opcode 三态覆盖矩阵产出；③ unbound 矩阵全绿；④ 最小端到端链路 CI 常绿（须在真实 M2 解释器上验收，不得以穿刺环境销项） |
| M5 | 直调/绑定方法免分配/builtin fastcall/kwargs/CALL_FUNCTION_EX | 不做调用点内联（v1.1） | 调用矩阵（285+，含 refcount 断言）全绿 |
| M6 | localsplus/操作数栈/指令位置恢复；异常表回退；guard 点级 forced-deopt fuzz；**异常注入 fuzz（JSC exception-fuzz 同款：首跑统计异常检查点数，再按序号注入合成异常，验证传播/捕获完整；注入开关实现于 cinderx 运行时层，暴露为环境变量/cinderjit API，与 forced-deopt 共享 count-then-inject 基建）**；tracing pause（D8） | 不做栈上帧强制回退；C 层旁路按待拍板① | ① 三种 deopt 策略×语料全绿且 ASAN 干净；② test_traceback/exceptions/sys JIT-all 通过；③ settrace 往返有测试；④ libtest 配置 3 进 daily；⑤ 异常注入 fuzz 对差分语料全检查点扫荡通过（固定种子可复现），已知问题 ⓪ 类以此销案 |
| M7 | 四类 IC version-guard（D5/D9）；global cache 拉式改造；变异套件（含 `__eq__`/`__del__` 再入）驱动 | 不引入回调式失效 | ① 变异矩阵全绿；② ASAN+refleak 重跑绿；③ del global 后旧值不可见专项 |
| M8 | sync gen send/throw/close/yield-from；挂起点 deopt 安全；协程/async gen 拒编测试 | 不编协程/async gen；gen 不走 LWF | ① gen 差分套件绿；② 挂起点 deopt ASAN 无内存错误；③ 拒编路径有测试 |
| M9 | helper heatmap；ratio vs 3.14-cinderx ratio（基线为发行版 PGO 二进制）；CALL/IC/BINARY/SUBSCR/FOR_ITER 热点收敛；timebox | 不翻 LWF；不加临时 hack；async 不追 | ① 一键热点归因；② 达成事先定死目标（归因工具出首批数据后定数）；③ 到期截断 |
| M10 | openEuler 交付物形态（rpm/内部 wheel 源）；安装/运维文档；支持矩阵对客声明（锚定 openEuler 24.03 3.11.6）；问题上报与热修流程 | 不做 PyPI 公开分发 | ① 干净 openEuler 环境一条命令可装可用；② 支持声明评审通过；③ 热修演练一次（打包→分发→客户侧升级路径） |
| 常驻 | sync 双跑；门禁加厚；白名单/基线裁决；openEuler errata 监控（核心文件集变更报警）；agent 产出评审 | — | sync PR 双版本 CI 绿方可合入 |

## 8. 当前状态（2026-07-02）

**M0 基本完成**，产物在 `ci_pipeline/diffgate/`（未提交；全部为可移植核心，直接在目标环境运行，不依赖特定开发机，docker 仅作为开发机上的挂载壳出现在 README 示例中）：
- 差分框架（run_diffgate.py/_bootstrap.py）：三模式、行级 diff、白名单 normalizer、崩溃恢复（SEGV 归因到 case 续跑）、基线判定（⊆ 基线即绿）；
- 语料 918 case（七模块：unbound/calls/operators/controlflow/ic_mutation/frames/hotloops）+ 基础 libtest 差分（run_libtest_diff.py，26 模块）；opcode 覆盖统计工具（tools/opcode_coverage.py，当前 3.11 范围内 98/98 全覆盖，async/import 类 deferred 带理由）；白名单 v1 + 钉住测试；（ASAN 门禁整体移至 M2，含脚本与首跑）；
- **穿刺失败基线已固化**（baselines/spike-cp311-20260629.json，75/347），确定性验证通过（两次全量逐 case 一致），带基线重跑 exit 0；
- 穿刺环境配方：分支 guo/cp311-stock-cinderx-adapt@1907f234（脏工作区）+ scratch/bootstrap311-20260629 预构建 .so + 镜像 cinderx-cp311-perf:20260627（注意：穿刺跑在 manylinux 3.11.15 上，重建时构建环境切到 openEuler 目标）。

**首轮基线的失败分类**（重建工作的已知问题清单，逐项销账）：⓪ **异常路径系统性损坏（最高优先）**——基础 libtest 差分（26 个核心模块，AUTO=24）26/26 全部发散，根因单一：任何热路径上"抛异常并被捕获"的代码触发 `SystemError: error return without exception set`（`re._parser`、`functools` 等 stdlib 处处命中），穿刺末次提交即在修此类问题、未完成；该 bug 在 pyperformance 冒烟中从未暴露，证明 libtest 差分层的必要性；① unbound 变量 5 个 SEGV + SystemError + NameError 变量名乱码（localsplus 映射存在系统性问题，疑与⓪同族）；② attr IC 对类变异完全不失效（多处返回过期缓存值）；③ frame/traceback/gen-close/递归 3+ 个 SEGV，f_lineno 读到空对象；④ CALL_FUNCTION_EX TypeError 缺 qualname 前缀；⑤ 1 个仅强制 deopt 下的 SEGV。

**M0 剩余**：diffgate_311.toml 挂入 run_gate；基线/白名单 owner 指定（流程决定）。（热循环 case 已完成；ASAN 移至 M2：穿刺不值得插桩，首个有价值对象是 M2 重建产物；硬约束是 M6 开工前 ASAN 必须可用。）

## 9. 风险预警：失控信号与触发动作

| 信号 | 触发动作 |
|------|----------|
| M2 libtest 等价失败清单连续 2 周不收敛 | 升级评审 vendor patch 范围，冻结新补丁 |
| M4 最小端到端第 4 个月末未绿 | 砍 M9 优化部分，禁止压缩 M6/M7 测试投入 |
| M6 同类字节码反复出 deopt 帧重建 bug | 该类临时进拒编名单，解耦排期 |
| M7 出现 ASAN UAF | 停新功能，审计全部 guard-before-deref 点 |
| agent 产出评审队列积压 | 降 agent 并行度，不降评审标准 |
| 门禁抓不住已知穿刺 bug | 停止重建开工，先修门禁 |
| 基线尺寸连续一个月不下降 | 里程碑评审：是排期问题还是能力缺口 |

## 10. 目标平台（openEuler 24.03-LTS-SP3）

- python3 = 3.11.6 + 约 50 补丁（Release 32，活跃更新）：40+ stdlib CVE 回移、架构补丁（loongarch64/sw_64）、发行版构建补丁。从命名看无核心解释器补丁，M1 机器验证定案。
- 构建配置对齐清单来源：`--enable-shared`、`--with-computed-gotos=yes`、`--with-dtrace`、LTO+PGO（x86_64/aarch64）。
- **性能基线注意**：A/B 基线是 PGO 优化后的解释器，M9 目标据此校准（账面加速比小于对裸构建）。
- **架构范围**：发行版含 x86_64/aarch64/loongarch64/sw_64/riscv64；JIT 仅覆盖 x86_64/aarch64，**其余架构 cinderx 必须干净自我禁用**（构建门控或运行时 no-op），这是 M10 交付验收的一部分。

## 11. 附录：近期可交给 agent 的任务

1. opcode 缓存/还原表一致性 verifier（M1 前置，纯工具）；
2. 热循环 case（M0 收尾）；
3. libtest stock oracle 记录 + JSON 比对器（复用 diffgate 基线格式，一两百行）；
4. `#if PY_VERSION_HEX` lint（软规则 + 存量基线豁免）；
5. SRPM 核心文件集 diff 验证脚本（M1 出口⑤）；
6. RuntimeTests 后端白名单首版（gtest filter 清单）；
7. test_cinderx 3.11 取舍清单落地（含 coroutine 测试改写为拒编断言）。
