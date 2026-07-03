# CPython 3.11 适配里程碑预演报告（M0–M1）

> 记录日期：2026-07-03（预备夜）｜ 分支：dryrun/m0-gates、dryrun/m1-build
> 基线：dryrun-311-base（kunpeng 仓 dev @75f0f552）
> 性质：工程预演。完成标准为“基本功能可运行 + 问题清单 + 工时记录”，
> 预演代码不直接进入正式开发。

## 0. MR 范围重构（2026-07-03 深夜二轮）

原 dryrun/m1-build 分支（81 文件 +8179 行）把验证分支的**全部**内容按整
体移植带入，混入了设计书 M1 之外的 M3/M4/M8 级功能（HIR 字节码翻译、
frame 运行时、auto-JIT 引导、功能测试）。按"MR 只做 M1"重构为：

- **dryrun/m1-build（MR #3，本分支）= 纯 M1**：构建门控 + 生成物 +
  borrow/垫片 + 共享代码 3.11 编译守卫 + 范围外空壳化 + **JIT 拒编
  安全阀**（3.11 上 `compileFunction`/eligibility 一律拒绝——3.11 形态
  为"可构建、可导入、JIT 惰性"）+ 哈希/SRPM 门禁原型。
  59 文件 +4216 行，其中约六成为生成物/桩/守卫/脚本；builder.cpp 从
  +1029 收敛到 +4（两处纯编译守卫）。
- **dryrun/m1-full-port（存档分支）**：完整移植内容原样保留，
  M2/M3/M4/M8 各里程碑从中按范围取料（翻译层、frame 运行时、
  auto-JIT 引导、311 功能测试均在其中）。
- 拆分方法论：以 kunpeng 基线为起点、编译器驱动，逐错误对照全量移植版
  判"纯守卫→照搬 / 功能实现→拒编桩"；共 6 轮编译迭代收敛。

## 1. 执行摘要

- **M0（测试与门禁基线）：完成。** 差分门禁框架、918 用例语料、基础 libtest
  差分、opcode 覆盖工具、允许偏差清单及钉住测试全部就位，已作为 MR #2 提交。
- **M1（3.11 构建打通）：完成（预演口径），四项补全全部销账（2026-07-03
  深夜补全轮）。** 验证分支移植后 3.11 编译错误 352→0；**完整 pip wheel
  流程打通**（cp311 wheel 39MB，新鲜 venv 安装+导入+force_compile 全过）；
  **3.14 反向回归修复归零**（118 错 → builder.cpp 两处合并伤 → 双版本
  编译+冒烟全绿）；哈希门禁与 SRPM diff 原型产出并跑出真实判决。JIT 编译
  后执行存在 frame 布局 SEGV（定位明确，归 M2/M4）。详见 §4 与 M1-log。
- **最高价值发现**：碰共享文件必然破坏 3.14（builder.cpp 合并引入 118 个
  3.14 编译错误），且部分错误源于“为 3.11 加的 #if 守卫破坏了 3.14 的
  switch 结构”——用铁证支持了设计书 §6 的**双版本编译门禁**必要性。
  补全轮进一步证明：118 错实为两处花括号级合并伤（其一在 `#if >=030E`
  内、3.11 编译永不可见），**只有多版本真实编译能扫出**。
- **D7 机器判定（补全轮新增）**：openEuler 24.03-LTS-SP3
  python3-3.11.6-31（52 发行版补丁）与上游 v3.11.6 在 19 个 JIT 核心
  文件上逐字节一致，**判决 vendor-from-upstream**——M2 的 ceval vendor
  与 borrow 源直接锚定上游 v3.11.6。

## 2. 六个必答问题的回答

| # | 问题 | 预演结论 |
|---|------|----------|
| 1 | vendored ceval 补丁数量与规模 | **未触及**：验证分支未复制 ceval（用 stock 循环 + PEP 523），M2 仍是唯一无参考区，本预演未推进到 M2 |
| 2 | Borrow 清单长度、生成 vs 手写取舍 | 清单 ≈30 符号（fallback.c +504 行）；预演中发现它**缺 `_PyThreadState_PopFrame`**（不在 3.11 动态符号表），需自行补实现（已完成，镜像 CPython pystate.c 语义）。补全轮评估：全部符号可在上游 3.11.6 源逐字对应，**生成管线原理上可行**，正式 M1 建议转 template+gen_cached，源锚定上游 v3.11.6（D7 判决支撑） |
| 3 | libtest JIT-off 等价失败规模 | **未推进到**（JIT 执行 SEGV 挡在前面）。M0 已备好 26 模块差分工具 |
| 4 | 6-MR 切分是否需调整 | **需调整**：M1 内部至少应拆为“移植（机械）/构建修复（手工）/范围外空壳化”三个可评审单元；builder.cpp 这类大合并文件应单独成 MR 并强制双版本编译 |
| 5 | 编码代理效率数据 | 移植+45 处冲突解决约 45 分钟；构建修复约 10 轮迭代约 2 小时。瓶颈在**编译-诊断-修复循环的串行等待**（每轮 3–5 分钟编译），非编写 |
| 6 | 计划外依赖/顺序问题 | 见 §3 修复分类账。核心：kunpeng 新子系统对 3.11 无守卫、三方合并机械伤、3.14 反向回归 |

## 3. M1 修复分类账（正式开发 MR 评审清单素材）

编译错误收敛：352 → 164 → 39 → 29 → 11 → 1 → 25(OSR) → 3 → 2 → 0（约 10 轮）。

1. **三方合并机械伤**（编译器/链接器能抓，但耗时）：公共 `}` 被吞（code_extra
   ×2、inline_cache、pyjit ×2）、公共 `#endif` 残留（code_patcher）、头文件
   声明与定义分道（compileFunction 默认参数 → 链接期 undefined symbol）。
   → 沉淀：**花括号+预处理器配平自检脚本**（与基线做增量对比过滤字面量噪音）。
2. **语义静默漂移**（编译器不一定抓）：python.h 的 atomic 头排序块被 kunpeng
   加了 `>=030C` 条件，自动合并后验证分支专为 3.11 写的 `#undef HAVE_STD_ATOMIC`
   变成死代码，错误爆在两层 include 之外。→ **版本条件的合并必须逐处人工确认**。
3. **命名漂移**：codeExtraIfPresent → codeExtraIfExists。
4. **kunpeng 新子系统无 3.11 意识**（本预演最系统性的一类）：
   behavior_classifier（331 个 case 标签需 #ifdef 包裹）、tree_iter pass
   （3.13+ dict 内联 values）、slot 快路径、inline_cache 的 PyDictOrValues 分支。
   其中 inline_cache 的守卫是 `<030E`，**在 3.14 上恒假、是死代码，所以从未
   在 3.14 CI 暴露**——只有真正的 3.11 编译尝试能扫出。
5. **3.11 符号缺口**：`_PyThreadState_PopFrame` 补 fallback 实现；
   `PyUnstable_Long_*`、`_Py_atomic_*_ptr/int` 系加 python.h 垫片。
6. **范围外子系统空壳化**（设计既定决策）：OSR 五函数（3.11 无 OSR）、
   AsyncLazyValue、anext builtins 补丁（与方法表守卫对齐）。

## 4. 四项补全的真实状态（2026-07-03 深夜补全轮全部销账）

- **① wheel 打包：完成。** 两个真实坑，均已修：
  1. opcodes 命名：正解是 `git mv 3_11 → 3.11` 对齐上游带点约定（纯路径
     访问、无包导入），setup.py 还原原样、零特例；
  2. `CINDERX_LOCAL_DEPS_DIR` 期望 `<dir>/<name>` git clone 布局（校验
     origin+tag），与 FetchContent 缓存布局（`fmt-src` 等）互斥——正式 CI
     的 deps 缓存必须按前者自建。
  结果：cp311 wheel（39MB）构建成功，新鲜 venv 安装+导入+init+
  force_compile 全过。**M1 出口①闭环**。
- **② 3.14 不破坏：已修复归零。** 118 错收敛为 builder.cpp 两处合并伤
  （emitLoadAttr 悬挂 `if {`、METHOD_WITH_VALUES case 缺 `}`——后者藏在
  `#if >=030E` 内 3.11 编译不可见）。修复后 3.14 全量编译（53/53）+
  import/force_compile/执行冒烟全绿，3.11 同源重编亦绿。**双版本编译门禁
  的必要性与可行性同时被实证**（M1 出口③预演口径达成）。
- **③ JIT 执行 SEGV**：**定位完成，状态无变化（补全轮复测复现如旧）**。
  崩在 `JITRT_UnlinkFrame` → `_PyFrame_ClearExceptCode`（fallback.c:282，
  `Py_CLEAR(frame->frame_obj)`），即 JIT frame 的 unlink/布局路径，高度
  怀疑与 co_framesize 替代公式相关。frame 模型深水区，归 **M2/M3/M4**。
- **④ borrow 生成 / 哈希门禁 / SRPM diff：完成。**
  - `verify_core_hashes.py`：7 文件锁定，注入验证通过（改一字节→exit 1→
    还原复绿），M1 出口④原型达成；构建期挂接留正式 M1；
  - `verify_openeuler_core_diff.py`：SRPM %prep 后 vs 上游 v3.11.6，
    19 核心文件逐字节一致 → **vendor-from-upstream 判决**（M1 出口⑤）；
    跨发行版 %prep 需垫 `%package_help` 宏（openEuler-rpm-config 专属）；
  - borrow 生成：≈30 符号全部可上游逐字对应，生成管线原理可行，
    正式 M1 转 template+gen_cached。

## 5. 对正式开发的具体建议

1. **M1 必须以“3.11 + 3.14 双版本全量编译通过”为出口条件**，不能只验 3.11——
   本预演证明单版本绿会放过大量反向回归（尤其 `<030E` 死代码守卫类）。
2. **builder.cpp（+1496 行，最大冲突文件）应单独成 MR**，逐块标注每个
   `#if` 在 3.11/3.12/3.14 三个版本下的花括号与分支平衡。
3. **验证分支的性能层（d4380b72）确认放弃移植**：其自建 IC 体系
   （LoadAttrCachedFastPath 等）与 kunpeng 现行架构互斥，M5/M7/M9 须基于
   kunpeng 架构重写，验证分支仅作行为参考。
4. **验证分支 watcher 全为空桩** → M7 的版本号守卫是真实从零工作量，
   inline_cache.cpp 的 687 行仅缓存结构可参考、失效机制不存在。
5. **配平自检脚本沉淀为 MR 前置检查**（花括号 + 预处理器 vs 基线增量）。
6. wheel 打包的 opcodes 命名一致性、borrow 生成管线、源码哈希门禁、SRPM diff
   四项均已在预演中打通或产出原型（见 §4），正式 M1 的剩余工程化工作：
   哈希门禁挂接构建期、borrow 转 template+gen_cached、deps 缓存按
   CINDERX_LOCAL_DEPS_DIR 布局自建、配平自检脚本沉淀（仍未做）。

## 6. 工时（预备夜实测，供 M1 估算校准）

| 阶段 | 实测 |
|---|---|
| 验证分支移植 + 45 处冲突解决 | ~45 分钟 |
| 构建修复（约 10 轮编译-诊断循环） | ~2 小时 |
| 补全调查（wheel/3.14/SEGV 定位） | ~40 分钟 |
| 补全轮（wheel 两坑修复+3.14 归零+两脚本+SRPM 管线） | ~1.5 小时 |
| **M1 预演合计** | **~5 小时** |

外推：正式 M1 加上双版本门禁、borrow 生成、哈希/SRPM、builder.cpp 逐块核对，
按设计书 0.5 人月估算基本合理，但 builder.cpp 双版本合并是被低估的单点，
建议单独留 buffer。
