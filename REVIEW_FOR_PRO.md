# GPT Pro Code Review：CPython 3.11.6 属性路径优化

## 基线与检视范围

**基线是 openEuler / Kunpeng CinderX 的 dev，不是本 GitHub 仓库的 main。**

- 固定基线：`bdba8d75f49d441c929981ffed5fdbfb9545ed58`
- 已构建、验证的代码头：`75cc87835b090e4912fc26c22c1be247b72f4352`
- 本 review 分支随后只有本说明和结果摘要的新增，不修改已验证代码。
- 原始来源：`https://gitcode.com/openeuler/cinderx` 的 dev。
- 当前代码头已包含五条检视意见的修复；详见 REVIEW_FIXES.md。提交范围包含文档与测试，不应把所有新增行视为纯实现。

```bash
git diff bdba8d75f49d441c929981ffed5fdbfb9545ed58 75cc87835b090e4912fc26c22c1be247b72f4352 -- CMakeLists.txt cinderx tools/attr_path
git log --oneline bdba8d75f49d441c929981ffed5fdbfb9545ed58..75cc87835b090e4912fc26c22c1be247b72f4352
```

主要代码提交依次是：
1. `910c9a53e`：移植两项属性优化。
2. `b7e9d0a2f`：补齐可移植全局缓存、JIT-off帧内调用与PGO训练。
3. `613ee37f8`：整理配置命名、拆分诊断接口、拆分patch并补说明。
4. `75cc87835`：修复实例字典异常、启动继承/失败、缓存所有权及sdist打包。

## 目标与边界

通过 CinderX 自定义解释器加速使用默认 __getattribute__、通过 __getattr__ 补查的类型。实现不写死 PyTorch 注册表，也不修改模型源码或宿主 CPython。当前验证目标是 Linux ARM、CPython 3.11.6、主解释器、eager、JIT-off。

本次新增的帧内调用放行只作用于 JIT-off。不要把已通过的 JIT-off 测试当作 JIT-on 联合兼容性的证明。这个分支是待检视实现，不是生产正确性已经闭环的承诺。

## 建议检视顺序

1. `cinderx/Interpreter/3.11/getattr_noexc.h`：普通缺失不构造临时 AttributeError；三态结果、handler快照、描述符绑定、引用计数、最终异常上下文。
2. `specialize_wrapper.c`、`specga.h`、`attr-patches/0001-specialize-getattr-only-types.patch`：放行资格、原守卫保留、一般键字典及共享辅助函数影响范围。
3. `attr-patches/0002-load-attr-with-hint-split-dict.patch`：split dict 的 ma_values 读取；键身份、hint范围、空值与失效。
4. `global_cache311.h`、`0003-global-name-cache.patch`：名称缓存的完整字典内容版本、借用值生命周期、遮蔽/重绑定、地址复用、回调与回退。
5. `ceval_wrapper.c`、`0004-jit-off-inline-call.patch`：求值器所有权、递归限制、trace/profile、参数绑定、JIT状态与已编译artifact保护。
6. `eval_hook.c`、`attr_config.h`、`PythonLib/_cinderx_auto.py`、`_cinderx-lib.cpp`：初始化失败与部分状态、重复安装/移除、配置快照、模式覆盖及新接口。
7. 顶层 `CMakeLists.txt`、`attr-patches/apply.cmake`：版本/构建路径范围、源文件哈希、patch变更的重配置、生成副本与失败处理。

四份patch的统一说明位于 `cinderx/Interpreter/3.11/attr-patches/README.md`。它们应用于构建目录副本，仓库中的upstream原文件未修改，但运行逻辑来自打补丁后的副本。

## 运行接口

```bash
ATTR_PATH_MODE=off python workload.py
ATTR_PATH_MODE=cinderx_control python workload.py
ATTR_PATH_MODE=cinderx python workload.py
ATTR_PATH_MODE=cinderx python -c "import _cinderx_auto; print(_cinderx_auto._verify())"
```

- off：原生对照。
- cinderx_control：保留自定义解释器和配套修复，关闭两项属性优化。
- cinderx：默认完整方案，强制JIT-off。
- ATTR_PATH_GLOBAL_CACHE / ATTR_PATH_INLINE_CALL：配套修复消融开关。
- ATTR_PATH_DIAGNOSTICS=1：诊断计数；计时关闭。
- `_attr_path_status()`：六个配置字段。
- `_attr_path_counters()`：诊断关闭时为None，开启时返回六项累计计数。
- `inline_call`只是开关，不是快路径已命中的证据。

配置及诊断接口已改变；旧wheel的混合 `_interp_getattr_stats311()` 接口不再保留。当前内部环境变量统一使用 ATTR_PATH_*，没有清理 dev 中其他功能既有的 SPIKE_* 配置。

## 修复前验证与性能历史（不代表修复后性能）

源码整理前后，两套patch生成的 ceval.c 和 specialize.c 字节一致；属性查询/全局缓存热点函数体及帧内调用逻辑不变。

最新wheel：`cinderx-2026.11.6.110603-cp311-cp311-linux_aarch64.whl`，PGO+LTO，训练生成159个profile文件，JIT关闭。轮次对应的编译/验证元数据见 `tools/attr_path/review_evidence/BUILD_AND_VALIDATION.json`。

通过：29项特化、43项属性语义差分、31项缓存检查、第三方求值器接管、配置快照开关两档，以及5个CPython测试模块共624项（跳过1项）。配置测试包含native加载后修改环境，再移除/重装。

同环境、固定CPU、独立进程，原生/整理前110602/整理后110603三档六种排列。每进程预热60次、7样本×15 forward；计时关闭诊断并暂停循环GC。模型源码、输入、config和mock轨迹一致，候选JIT计数为零。没有真实NPU数值回归，这些时间不是实机FREE。

|模型|110603相对原生耗时降幅|相对110602|
|---|---:|---:|
|DLRM|10.93%|慢0.25%|
|MMOE|13.68%|快0.44%|
|ESMM|13.37%|快0.37%|
|GR 2B|-0.03%|快0.37%|

四项新旧差异的六组配对bootstrap参考区间均跨零；这不是严格的性能等效证明。详细配对样本摘要在同目录 SUMMARY.json。完整原始日志已在本地交付包保留，没有上传wheel和大规模性能采样到本仓库。

## 希望 Review 输出什么

请优先找能构造失败场景的正确性问题，再评估性能和维护性。每条指出准确文件/函数/行号、触发条件、可能后果、最小复现思路及修复建议；区分源码推断与已经运行证实。不要仅因存在某种高级机制就认定有收益或有缺陷。

重点关注：异常处理是否丢失可观察行为；类型/描述符/字典变异；缓存借用引用与版本契约；求值器所有权及重入；帧内调用是否漏掉原入口职责；新配置捕获时机；初始化/移除失败；patch和构建入口覆盖。测试通过不等于这些边界全部证明完毕。

工程化待审项目包括：初始化/清理顺序、其他Python版本及独立构建入口、patch变更重配置、补丁后内容校验、消融接口是否应收敛。请按明确的支持范围定级，不把所有未测试平台自动视为已发生的缺陷。
## 本轮修复验证

请先阅读 REVIEW_FIXES.md 及 tools/attr_path/review_evidence/REVIEW_FIXES_VALIDATION.json。修复后通过真实 .pth 子进程矩阵和对应正确性回归，并从解包的 sdist 构建 wheel。首轮修复验证使用 Release+LTO；后续110605已完成PGO+LTO对比，发现约0.5%–1.1%的小幅回退，详见REVIEW_FIXES.md后续章节。
