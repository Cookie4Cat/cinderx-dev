# M1 预演记录：3.11 构建打通

> 起始：2026-07-03 20:21 ｜ 分支 dryrun/m1-build ｜ 基线 dryrun-311-base（kunpeng dev 75f0f552）

## 移植决策与冲突数据（问题④⑥的素材）

验证分支三个提交的移植结果：

| 提交 | 处置 | 冲突 | 说明 |
|---|---|---|---|
| 3bf80112（核心适配，68 文件） | **已移植** | 45 处 / 20 文件 | 冲突全部为"kunpeng 近期重构 vs 验证分支基于旧结构的加法"，无语义矛盾 |
| d4380b72（性能优化层） | **放弃移植** | 40+ 处（架构级） | 验证分支自建的 IC 体系（LoadAttrCachedFastPath、VectorCallTstate、TypeDescrOrClassVar 等）与 kunpeng 现行 IC 架构（SplitInlineKnownOffset、isCallLike 等）互斥。结论：**正式开发的 M5/M7/M9 应基于 kunpeng 现行架构重写 3.11 支持，验证分支性能层仅作行为参考，不可移植** |
| 1907f234（异常路径修复） | **已移植** | 4 处 | 含 3.11 deopt 帧重建（reifyLocalsplus，207 行）与 auto-JIT 引导修复 |

冲突解决模式统计（可指导正式开发的 MR 评审预期）：
- "双方均为加法，直接并存"：约 60%；
- "kunpeng 重构胜出，验证分支同类实现丢弃"（code_patcher 缓存刷新、postgen 变换、pyjit auto-JIT 调度、全局缓存守卫）：约 25%；
- "版本守卫分流"（#if <0x030C 走验证分支路径 / #else 走 kunpeng 路径）：约 15%；
- 特有陷阱：**悬挂 #if 模式**——双方各自新增以同一个公共 #endif 结尾的版本守卫块，
  三方合并时需要手工补 #endif 缝合，出现 4 次（bytecode.cpp×2、jit_rt.cpp、builder.cpp）。

## 遗留问题清单

1. `_cinderx_auto.py`：kunpeng（3.14 插件引导，294 行）与验证分支（3.11 auto-JIT
   引导，399 行）各有整套实现，预演取 3.11 版；**正式开发需设计双版本合流**；
2. `compileFunction` 多目标编译（验证分支的依赖预加载编译）被 kunpeng 单目标签名
   取代，auto-JIT 行为可能有差异，M4 阶段复核；
3. 验证分支的 macOS MAP_JIT 支持已保留（code_allocator.cpp），目标平台仅 Linux，
   正式开发时评估是否裁剪；
4. deps 缓存复用自验证分支构建目录（fmt/googletest/phmap/usdt），正式 CI 需自建
   `CINDERX_LOCAL_DEPS_DIR` 缓存。

## 构建迭代记录

- 20:21 开始移植；约 21:05 完成两个提交（45+4 处冲突）；
- 21:1x 首轮构建启动（容器 dryrun-m1，gcc-14 工具集，python 3.11.15，
  CINDERX_LOCAL_DEPS_DIR=/deps）——结果待记。

## 构建期发现（追加）

5. **重大情报——验证分支的 watcher 全部为空桩**（cinderx/python.h：四类
   AddWatcher/Watch 一律返回 0 的空实现）。含义：验证分支的内联缓存在 3.11 上
   **从未有过失效机制**，差分门禁抓到的第 2 类缺陷（缓存过期值）不是实现 bug
   而是桩的必然结果。结论：M7 的版本号守卫方案（设计书 D5）是从零起步的真实
   工作量，验证分支在此处没有可参考的失效实现（inline_cache.cpp 的 687 行参考
   价值需重新评估——其缓存结构可参考、失效机制不存在）；
6. 合并陷阱补遗：cherry-pick 自动合并的"语义静默漂移"——kunpeng 在 python.h
   给 atomic 头文件排序块加了 `>= 0x030C` 条件（排除 3.10），自动合并后验证分支
   专为 3.11 写的 `#undef HAVE_STD_ATOMIC` 变成死代码，编译在深处的
   pycore_atomic.h 报错。教训：**自动合并成功 ≠ 语义正确，版本条件的合并必须
   逐处人工确认**（正式开发 MR 评审清单素材）；
7. 命名漂移一例：codeExtraIfPresent（验证分支）→ codeExtraIfExists（kunpeng）；
8. 合并悬挂陷阱实计：公共 `}` 被吃 ×2（code_extra.h）、公共 `#endif` 残留 ×1
   （code_patcher.cpp）；配平自检脚本（花括号+预处理器 vs 基线增量）应沉淀为
   正式开发的 MR 前自检工具。

## M1 收尾（预备夜实测结果）

- **出口达成**：JIT 禁用形态下 `import cinderx` 成功、cinderjit 注册、基本执行正常
  （python 3.11.15 容器，gcc-14）；显式 force_compile 单函数编译成功。
- **遗留（归 M2/M4 层）**：执行 JIT 编译后的函数即段错误（faulthandler 定位在被编译
  函数的首次调用处），gdb 原生回溯待抓——这是编译代码运行正确性问题，正是后续
  里程碑的主战场，预演按计划前进。
- **编译错误收敛曲线**：352 → 164 → 39 → 29 → 11 → 1 → 25（OSR 层）→ 3 → 2 → 0，
  共约 10 轮迭代。
- **修复分类账**（正式开发 MR 评审清单素材）：
  1. 合并机械伤：公共 `}` 被吞 ×3（code_extra ×2、inline_cache、pyjit ×2 计 5 处）、
     公共 `#endif` 残留 ×1、头文件声明与定义分道（compileFunction 默认参数）；
  2. 语义静默漂移：python.h atomic 块版本条件被上下文改变含义；
  3. 命名漂移：codeExtraIfPresent→codeExtraIfExists；
  4. kunpeng 新子系统无 3.11 意识：behavior_classifier（331 个 case 标签 #ifdef 包裹）、
     tree_iter pass（3.13+ dict 内联 values）、slot 快路径（030E 助手）、
     inline_cache 的 PyDictOrValues 分支（`<030E` 守卫在 3.14 上是死代码所以从未暴露）；
  5. 3.11 符号缺口：_PyThreadState_PopFrame 不在动态符号表 → fallback.c 补实现
     （镜像 CPython pystate.c 语义）；PyUnstable_Long_*、_Py_atomic_*_ptr/int 系加垫片；
  6. 范围外子系统空壳化：OSR 四函数（3.11 无 OSR 为既定决策）、AsyncLazyValue、
     anext builtins 补丁（与方法表守卫对齐）。
- 工时：预备夜合计约 3 小时（移植+冲突 45 分钟、构建迭代约 2 小时）。

## M1 补全轮（2026-07-03 深夜，四项补全销账）

### ① wheel 打包：完整 pip wheel 流程打通

- **opcodes 命名坑的正解是改目录不改 setup.py**：上游约定是带点命名
  （opcodes/3.12、3.14、3.15，纯路径访问、无 __init__.py），验证分支的
  `3_11` 下划线命名是无必要的偏离（无任何代码按模块导入它）。
  已 `git mv 3_11 → 3.11`，setup.py 还原上游原样，零特例。
- **第二个真实坑：CINDERX_LOCAL_DEPS_DIR 与 FetchContent 缓存布局互斥**。
  该机制期望 `<dir>/<name>` 为 git clone（校验 origin+tag，不匹配即删掉重
  clone）；挂载的 /deps 是 FetchContent 布局（fmt-src/fmt-build）且只读 →
  configure 期直接 fatal。预演解法：容器内可写目录按期望布局播种
  （fmt-src 拷为 fmt 并 checkout 到钉住 tag 11.2.0；phmap/usdt 已在钉住
  commit）。正式 CI 的 deps 缓存必须按 CINDERX_LOCAL_DEPS_DIR 的 clone
  布局自建，不能复用 FetchContent _deps 目录。
- 结果：`cinderx-2026.7.3.0-cp311-cp311-linux_aarch64.whl`（39MB）构建成功；
  新鲜 venv pip install + import + cinderx.init() + cinderjit 注册 +
  force_compile 全通过。**M1 出口①闭环**。
- 已知遗留复测：JIT 编译后函数首次调用 SEGV 复现如旧（faulthandler 定位
  一致），归 M2/M4，状态无变化。

### ② 3.14 反向回归：118 错 → 0，双版本编译+冒烟全绿

- 118 个 3.14 错误收敛为 builder.cpp 内**两处独立合并伤**：
  1. emitLoadAttr 前奏：3.11 守卫把 3.12+ 侧留成悬挂 `if (oparg & 1) {`
     ——修复为最小内联 `#if`（3.12+ 逐字节还原 kunpeng 基线，3.11 侧
     `is_method=false`，LOAD_METHOD 在 3.11 是独立 opcode）；
  2. LOAD_ATTR_METHOD_WITH_VALUES case 块缺闭合 `}`（"公共 } 被吞"再+1，
     且该 case 在 `#if >=030E` 内，3.11 编译永不可见，**只有 3.14 真实编译
     能暴露**——双版本门禁必要性的又一实证）。
- 3.14 全量编译通过（ninja 53/53），import + force_compile + 执行冒烟正确；
  3.11 侧同源码重编同样通过（wheel 构建即证）。**M1 出口③（移植期口径）
  在预演层面达成**。
- 教训沉淀：两处伤均属"花括号+预处理器配平"类，配平自检脚本（vs 基线增量）
  仍未沉淀，正式开发 MR 前置检查必须补上（ci_pipeline/scripts/
  check_clean_code_incremental.py 是 clang-format/tidy 检查，不是配平脚本）。

### ④ 三小件：哈希门禁 / SRPM diff / borrow 评估

- **verify_core_hashes.py**（ci_pipeline/scripts/）：锁定 3.11 vendored/
  generated 面（Interpreter/3.11 五文件 + opcodes/3.11/opcode.py +
  borrowed-3.11-fallback.c，共 7 文件），清单 ci_pipeline/core_hashes_311.json。
  注入验证通过：改一字节 → exit 1 报 HASH MISMATCH → 还原复绿。
  构建期挂接点（setup.py BuildExt / CMake custom target）留给正式 M1。
  **M1 出口④原型达成**。
- **verify_openeuler_core_diff.py**（ci_pipeline/scripts/）：openEuler
  24.03-LTS-SP3 python3-3.11.6-31（52 补丁，%prep 后）vs 上游 v3.11.6，
  **19 个 JIT 核心文件逐字节全一致，判决 vendor-from-upstream**——D7 的
  M1 机器验证定案，M2 ceval vendor 直接取上游源。报告落盘
  scratch/openeuler-core-diff-report.json。**M1 出口⑤达成**。
  - 跨发行版 %prep 坑：spec 用 openEuler 专属宏 `%package_help`
    （openEuler-rpm-config 提供），AlmaLinux/manylinux 上需垫等价展开
    才能解析；正式管线在 openEuler 容器内跑则无此问题。
- **borrow 生成评估**：fallback.c 含 ≈30 个符号，全部可在上游 3.11.6 源中
  逐字对应（datastack 块助手=pystate.c 静态、行表机械=codeobject.c、
  _PyThreadState_PopFrame 在源中存在只是不出动态符号表）。结论：
  borrowed-3.11.c.template 生成管线**原理上可行**，正式 M1 建议转为
  template+gen_cached（可审计），源锚定上游 v3.11.6（依 D7 判决）。

### 工时（补全轮实测）

约 1.5 小时（wheel 两坑定位+修复 ~40 分钟、3.14 双版本收敛 ~20 分钟、
两脚本+SRPM 管线 ~30 分钟）。M0–M1 预演累计 ≈ 5 小时。

## MR 范围重构轮（同夜二轮）：纯 M1 分支的构造记录

背景与结果见 REPORT §0。过程记录（正式开发做同类拆分的配方）：

1. **处置三分法**：81 个移植文件逐个判为
   整取（构建门控/生成物/borrow/垫片/机械守卫/空壳化，49 文件）、
   起草（混合文件从 kunpeng 基线重造 compile-only 版，11 文件）、
   停车（功能层+功能测试，归 dryrun/m1-full-port，21 文件）；
2. **编译器驱动起草**：起草文件保持基线，跑 3.11 编译收集错误，逐错误
   对照全量移植版：纯守卫（API 可用性 `#if`）→ 照搬；功能实现（如
   super_lookup_builtin、compact-long 3.11 实现、_PyEval 恢复语义）→
   换 `JIT_ABORT`/拒编桩。6 轮迭代（29→16→4→2→0 错）；
3. **拒编安全阀**：`compileFunction`/`compileFunctionWithOSR`/
   `getCompilationEligibility`×2 四点 `<030C` 直接拒绝 + `_cinderx_auto.py`
   3.11 不引导 cinderjit——M1 出口语义从"force_compile 能编译（但执行
   SEGV）"修正为"**一律安全拒编**"，SEGV 从 M1 的账上消失（它属于带
   翻译层的里程碑）；
4. **整取件复审抓到的范围泄漏**：code_extra.h 的 auto-JIT flags 字段、
   code.cpp/h 的 codeAutoJitDisabled 助手（auto-JIT 簿记，功能层）——
   已回退/剥离。教训：**"整取"也要过一遍"这行为谁服务"**；
5. 处置有争议时的判据：这行代码在"JIT 永不编译"的 3.11 上是否仍会被
   执行到？会（加载/初始化/自省路径）→ M1 必须正确；不会 → 桩。
