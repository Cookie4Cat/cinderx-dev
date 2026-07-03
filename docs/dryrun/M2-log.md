# M2 预演记录：3.11 解释器循环适配（vendored ceval）

> 起始：2026-07-04 ｜ 分支 dryrun/m2-interp ｜ 基线 dryrun-311-base（M1 已合入 @9b1ff2026）
> M2 是设计书里最大的里程碑（正式估 3.5 人月）、唯一无答案书区域（穿刺未 vendor ceval）。

## 方案落地形态（D3 的工程化）

- **pristine 层**：`Interpreter/3.11/ceval/` 七文件（ceval.c 7946 行、specialize.c
  2083 行、frame.c 170 行 + ceval_gil/opcode_targets/condvar/pydtrace 头），
  **逐字节 == 上游 v3.11.6**（机器 diff 验证过），哈希门禁锁定（现锁 20 文件）。
- **补丁层 = wrapper TU**：`cinderx_ceval.c` / `cinderx_specialize.c` /
  `cinderx_frame.c`——每条偏离一个宏 + 理由，包含 pristine 源；补丁数：
  - **P1** 入口改名（`_PyEval_EvalFrameDefault → Ci_EvalFrameDefault_311`，
    文件内递归引用随宏一致改写）；
  - **P2** dict 版本号影子计数器（见下）；
  - specialize wrapper 认领 `NEED_OPCODE_TABLES`（上游同机制，选定义 TU）。
- **借用层**：`extract_ceval_extras.py` 迷你 borrow 生成器（UpstreamBorrow
  哲学：可再生成、regenerate-and-diff 审计）——从上游逐字抽取 **34 个**
  未导出私有助手（frame 三件套、dict 查找闭包 9 个、异常组 11 个、
  datastack 4 个、genobject 5 个等），含 typedef 抽取与节前宏。
- **shim 层**：`cinderx_ceval_shims.c` **9 个语义等价 shim**，每个附等价性
  论证（nb_ 槽直调=libpython 同一函数指针、audit "O" 格式转发、mmap 等）；
  等价性最终由配置②差分门禁裁决，不靠 review 口头认定。

## 必答问题 1 的实测答案（borrow 清单规模）

加载器实测：vendored 循环在 **manylinux（静态链接 libpython）** 上缺
**44 个符号**。处置分布：整文件 vendor 2（specialize/frame）+ 逐字抽取 34
+ 语义 shim 9 + 影子发号器 3。**关键限定：44 是悲观上界**——openEuler
目标用 `--enable-shared`，绝大多数 `_Py*` 内部符号会在动态符号表里，
正式开发在 openEuler 容器重测预计 borrow 面大幅缩小（可能近零）。

## 最危险的坑：版本号发号器三兄弟（正式开发必读）

`_pydict_global_version`（uint64，PEP 509 ma_version_tag）、
`next_dict_keys_version`（uint32，dk_version）、`next_func_version`
（uint32，func_version）都是 libpython 文件内静态计数器。**天真地自建
从 0 起步的副本会与运行时发号器撞号 → 版本守卫把"已变异"判成"未变"
→ 缓存腐败**（正是 D5/M7 的地基）。预演处置：
- uint64：探针 dict 读出运行时当前值，影子播种到 +2^40；
- uint32×2：上下半区分割（影子从 2^31 起步）。
均记录于生成器/ wrapper 注释。**openEuler 共享 libpython 导出这些符号，
正式开发直接外链真发号器，三个垫片全部消失**——此坑为 dry-run 环境特有，
但"发号器不可复制"的分析对 M7 的 IC 设计是永久有效的约束。

## index-进缓存的 shim 红线（评审原则素材）

`_PyDict_GetItemHint` 返回的 index 会写进 LOAD_ATTR/LOAD_GLOBAL 内联缓存；
module 路径的 handler 以 keys_version 守卫后**直接按 index 取槽、无
me_key 复核** → 假 index + 命中的 keys_version = 静默错值。所以 dict
查找必须抽真实现（9 个静态的闭包），**不可用"公开 API 等价"shim**。
判据沉淀：**返回值会被缓存并在守卫命中后免检使用的函数，禁止行为近似
shim**。

## M1 遗留桩的清算

M1 fallback 里两个 NotImplementedError 桩被逐字版取代：
`_PyFrame_MakeAndSetFrameObject`（帧物化——注意它紧邻 M1 SEGV 的
`JITRT_UnlinkFrame→_PyFrame_ClearExceptCode` 区域，桩质量是 M3/M4 排查
该 SEGV 时的头号嫌疑）与 `_PyAsyncGenValueWrapperNew`（async gen 值包装，
libtest 的 test_compile_top_level_await 直接抓到）。教训：**fallback 的
"实现"必须逐项核对是真实现还是桩**——M1-log 已加"整取也要问这行为谁
服务"，本项再加"桩必须显式登记，禁止静默 NotImplementedError"。

## 环境 micro 漂移（预演环境特有，已固化基线）

容器运行时 3.11.13、D7 锚点 v3.11.6，ceval 相关漂移仅 3 个后移 bugfix
（35 行）。实测命中 1 个：gh-112716（`__builtins__` 非 dict 的
SystemError 修复，ceval.c + test_builtin.py 同补丁改动）→
`test_exec_builtins_mapping_import` 在"3.11.13 测试套 + 3.11.6 循环"下
必然发散。**D7 纪律：不采纳后移修复**；openEuler 3.11.6 目标环境
（循环 micro == 运行时 micro）无此项。已入基线
`baseline-m2-cp31113-microdrift.json`（rationale 即本节）。

## 验证结果

- **路由证实**：gdb 断点 `Ci_EvalFrameDefault_311` 命中，回栈
  PEP 523 → vendored 循环；`install_frame_evaluator()` 为配置②的显式开关
  （JIT 拒编形态下不自动安装——diffgate ②模式注入需带上这一句）。
- **libtest 26 模块差分（配置② vs stock）**：首跑 **25/26 等价**（对比
  穿刺时代 26/26 全发散）；2 个失败根因各一（async gen 桩→已修复；
  micro 漂移→基线）；最终相对基线全绿。
- 3.14 反向回归：全量重编 0 错（M2 改动全部在 3.11 专属文件 + CMake
  3.11 分支内）。

## 工时（实测）

vendor+wrapper+CMake ~30 分钟；缺符号收敛（16 轮编译/加载迭代，含抽取器
三次修正：前置声明误匹配、注释内散文误匹配、同行返回类型）~2 小时；
libtest 差分与两失败根因定位 ~40 分钟。**M2 预演合计 ≈ 3.5 小时**。
外推：正式 M2 的 3.5 人月主要花在等价性收敛与 refleak/全量 libtest 燃尽，
构建打通本身（本预演已给出配方）约占一成。

## 遗留（正式 M2 清单）

1. openEuler 容器复测 borrow 面（预计大幅缩小）+ 发号器直链；
2. refleak 抽样（regrtest -R）未跑；`_CiOpcode_*` verifier 未接；
3. diffgate ②模式接入（工具在 m0-gates 分支，本轮用 scratch 拷贝打
   `install_frame_evaluator` 补丁跑通——该补丁应回流 M0 工具）；
4. 抽取器三个匹配 bug 的测试（生成器自身需要钉住测试——"看住看门人"）；
5. 全量 libtest（本轮 26 模块语言核心子集）+ 构建配置对齐清单
   （computed-gotos ✓ 自动跟随 pyconfig.h；dtrace 空转 ✓；LTO/PGO 未对齐）。
