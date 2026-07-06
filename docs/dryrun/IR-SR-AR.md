# CinderX JIT（CPython 3.11）需求分解：IR → SR（终稿 v2.0）

> 定稿日期：2026-07-06。本文档为需求系统录入底稿：IR 与三条 SR 给出完整
> 需求描述与验收标准，AR 仅给出分解参考名，内容随后续任务定义。

**分解原则**：SR 面向流程验收——验收人可独立执行、结果可肉眼判定；研发
内控手段统一沉淀为 SR3 的门禁构成，不散布于 SR1/SR2 验收面。

**术语**："双模一致"指同一解释器 JIT 开启与关闭两种模式下运行结果一致。

---

## IR　原始需求

基于 pyperformance 用例集，20 个以上 JIT 用例性能相比 stock CPython
提升 20%。

**口径约定**：提升 20% = 固化清单基准的几何均值加速比 ≥ 1.20，且单项
最差 ≥ 0.90；基准清单（≥20 项）以附件固化，不得事后增删；测量协议 =
PGO+LTO 正式构建、全量编译、pyperformance 官方跑法、目标硬件。

---

## SR1　JIT 执行语义正确性

**需求描述**：JIT 启用后，Python 程序执行结果与 CPython 解释器一致
（语义等价），长时间运行无崩溃。

**验收标准**：

① 附件 A 固化的标准库测试模块清单（61 个）在 JIT 开启下运行，双模
一致，零新增差异；差异项仅允许两类——环境/上游版本漂移、经评审签认的
豁免项，豁免清单随验收报告归档；

② pyperformance 全量用例连续 3 轮全部正常完成，零崩溃。

**验收纪律**：附件 A 于开发启动前定稿；测试失败后模块不得移出清单，
只能走豁免评审；清单外测试面由 Daily 门禁（SR3）覆盖，不作 SR 验收
承诺。

**AR 分解参考**：编译管理与字节码语义编译 / 内联缓存与失效 / 去优化与
现场重建 / 帧与生成器。

### 附件 A　语义一致性测试模块清单（CPython 3.11，共 61 个）

**语言构造（12）**：test_grammar、test_call、test_scope、test_raise、
test_with、test_augassign、test_keywordonlyarg、test_unpack、
test_unpack_ex、test_compare、test_binop、test_fstring

**内建类型与容器（13）**：test_types、test_builtin、test_int、
test_float、test_bool、test_complex、test_unicode（3.11 中 str 测试的
模块名）、test_bytes、test_list、test_tuple、test_dict、test_set、
test_slice

**推导式与迭代（7）**：test_listcomps、test_dictcomps、test_setcomps、
test_genexps、test_iter、test_itertools、test_operator

**异常（2）**：test_exceptions、test_exception_group

**对象模型与对象协议（10）**：test_descr、test_super、test_property、
test_isinstance、test_abc、test_functools、test_weakref、test_gc、
test_pickle、test_copy

**生成器/协程、帧与 code 对象（10）**：test_generators、
test_yield_from、test_coroutines、test_asyncgen、test_frame、
test_code、test_inspect、test_traceback、test_sys_settrace、
test_sys_setprofile

**模块与导入（3）**：test_module、test_import、test_contextlib

**运行时联动与调试（4）**：test_sys、test_threading、test_pdb、
test_bdb

（全部模块名已对照 CPython 3.11.13 实际测试表核实；test_inspect 在
3.11.13 中为测试包，regrtest 仍以 test_inspect 名执行。）

---

## SR2　性能达标

**需求描述**：固定测量协议下，固化清单中 ≥20 个基准相对同版本 stock
CPython，几何均值加速比 ≥ 1.20，单项最差 ≥ 0.90。

**验收标准**：

① A/B 报告与原始数据归档，测量脚本可复现；

② 三次独立测量几何均值波动 < 2%；

③ 基准清单以附件固化，作为验收唯一依据。

**AR 分解参考**：帧与调用开销消减 / 内联缓存加速 / 热点代码内联与运算
特化 / 编译策略与协同执行。

---

## SR3　构建与质量门禁工程化

**需求描述**：提供可重复执行的正式构建体系与 PR 级、Daily 级两层质量
门禁。

**验收标准**：

① **构建**：干净环境按交付文档执行构建脚本，一次成功产出制品；制品带
版本标识、可追溯至源码 commit；正式构建（PGO+LTO）与 ASAN 构建两种
配置均可产出；

② **PR 门禁（合入强制，小时内完成）**：差分门禁（语料级双模差分）、
run_gate 三件套（RuntimeTests、test_cinderx、标准库差分快速档 = SR1
附件 A）、引用计数矩阵与冒烟集，随 MR 自动执行，任一失败阻断合入、无
人工旁路。验收动作：正反两用例——正常变更绿灯合入，植入已知缺陷的变更
（样例随附）红灯阻断；

③ **Daily 门禁（每日定时，失败自动告警至责任人）**：ASAN 构建下全量
run_gate、标准库宽口径差分（语义相关全集约 108 模块）、浸泡与泄漏检测、
pyperformance 性能回归比对。验收动作：抽查连续 7 天执行记录完整；人工
触发一次失败演练，确认告警送达。

**AR 分解参考**：正式构建（含 ASAN 配置）/ PR 合入门禁 / Daily 门禁与
浸泡。

---

## 决策记录

1. **内存泄漏不设 SR 级验收**：泄漏为趋势属性，证明手段（长驻浸泡、
   两段迭代比对）落入 SR3 Daily 门禁；备选方案 pyperf --track-memory
   峰值内存比值（实测 richards 1.12x）留作评审补充项。
2. **可观测性不设独立 SR**：tracing/调试支持采用回退解释器方案（与
   Meta CinderX 一致）；帧自省一致性由 SR1 附件 A 第六、八组模块覆盖；
   若后续启用轻量帧，自省回归随对应 AR 管理。
3. **标准库验收采用固化清单（61）而非全套（483）**：与 Meta CinderX
   选择性执行实践一致；全套含大量与 JIT 无关且环境敏感的模块，会引入
   验收争议；语义相关宽口径（约 108 模块）由 Daily 门禁覆盖。
   test_signal、test_faulthandler 因容器/虚拟化环境时序敏感，归 Daily
   不进附件 A。
4. **JIT 总开关**作为功能归属 SR1 首条 AR（验收含"关闭后行为等同
   stock"）；运维手册与诊断能力作为交付物随工程交付，不占 SR 验收面。
5. **性能验收全部阈值**（≥1.20 / ≥0.90 / <2%）与两份附件清单（基准
   清单、附件 A）于评审会确认后固化。
6. **ASAN 构建为净新增项**（预演未覆盖），构建配置与误报豁免清单需
   从零建立，排期单列、不按存量估算。
