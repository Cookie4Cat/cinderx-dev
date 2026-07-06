# M10 第六轮：PGO-use 相"错译"根因专项（判决翻案）

日期：2026-07-07　分支：`dryrun/m10-pgo-rootcause`　基线：T1 轮
（#41 合入后，纯 LTO + interpreter -O3）

## 一、对象与存量证据（C3 轮案三）

缺陷画像：PGO+LTO 三相构建中仅 PGO-use 相产物概率性出现"JIT 编译
码 raise 丢异常"（NULL 返回无异常上浮成 SystemError，import enum
即触发）。存量证据：plain/纯 LTO/插桩相全绿；同源码重训重建一红一
绿；IC 缓存全关仍红；单函数 jit-list（EnumType.__getattr__）可触
发；命中面随 profile 变动（另一红态 enum 过而 test_slice 炸）。
C3 轮判决为"GCC-14 PGO×LTO 工具链概率性错译"，交付口径退纯 LTO。
**本轮结论：该判决推翻——根因是移植层缺陷，工具链无罪。**

## 二、红态采样捕获

交付配方（build_pgo_lto_311.sh）原样循环、每轮独立重训（训练非
确定性即采样源）。**首轮即红**（全链 204 s）：相四探针红，gcda
全集（161 TU）+ 产物（md5 db06cb…）双重归档（/tmp/pgo-red-*、
/tmp/pgorc/red-gcda.tgz）。失败签名与 C3 一致：import enum 途中
`SystemError: error return without exception set`，探针五模块全红。

## 三、定界与验尸（红态 .so 隔离运行位，ASLR 关闭全程确定）

排除线（由外向内）：

1. **纯解释态（auto=0）全绿**——故障面在 JIT 侧；
2. **jit-list 单函数复现确认**——只编 EnumType.__getattr__ 即红；
3. **生成码本体无罪**：force_compile 对称转储，同 .so 两次逐位一致；
   失败语境（auto=2 期中编译）产物与 force_compile 产物助记符流
   前 814/820 条一致（尾差为跳岛/填充），且两者事后行为探针均能
   正确 raise——错误行为是调用路径/状态依赖，非机器码本体；
4. **异常时序**：全程记录 _PyErr_SetKeyError / PyErr_Clear /
   PyErr_Fetch / PyErr_Restore（带调用者 LR），致命序列为
   `SetKeyError → （无任何 Clear/Fetch/Restore）→ 无异常到达
   error 标签冷块`——异常在设置与检查之间"凭空蒸发"；
5. **冷块直捕**（error 标签无异常分支的机器码地址断点，唯一真事件
   触发点）：tstate 寄存器（x25）与 _PyRuntime.gilstate.
   tstate_current 一致，curexc 三元组确为全零；**帧 =
   enum.__getattr__、prev_instr 指函数首指令；栈上
   resumeInInterpreter（gen_asm.cpp:402）在案**——deopt 恢复路径
   就是案发现场；
6. **实参捕获**：resumeInInterpreter 入口，良性 deopt
   `is_instrumentation_deopt=0`，致命 deopt **`=72`（bool 收到
   0x48 脏值）**；
7. **x24 审计网**（prepareForDeopt 体内全部 bl 返回址布点）：致命
   deopt 全程 w24=0，返回序列 `bfxil x1,x24` 干净归一——
   prepareForDeopt 返回的 bool 是干净的 0，**脏值产于两个 C++ 调用
   之间的 JIT deopt 垫片**。

## 四、根因与修复

**根因**：`GenerateDeoptTrampolineBlocks`（cinderx/Jit/lir/
generator.cpp）中 resumeInInterpreter 第四实参
（is_instrumentation_deopt，自 prepareForDeopt 的辅助返回寄存器
搬运）的装配被 `#if PY_VERSION_HEX >= 0x030C0000` 版本门跳过，而
resumeInInterpreter 与 prepareForDeopt 的四参/结构返回签名在 3.11
无门、语义照常生效。于是 3.11 下垫片调用 resumeInInterpreter 时
**x3 为 prepareForDeopt 返回后的未定义寄存器残值**（本红态实测
0x48，恰为 DeoptMetadata 结构步长的寻址残留）。残值非零即被当作
instrumentation deopt：kRaise 的恢复流程绕过"回 RAISE_VARARGS 重
执行"，解释器以无异常状态进入 error 标签，SystemError 兜底触发。
该版本门经上游社区合并（4e173c401）流入；Meta 已不再构建 3.11，
故上游从未暴露。x86_64 的 3.11 构建同险（RCX 残值）。

**为何呈现"PGO 概率性错译"**：残值取决于各构建中 prepareForDeopt
的寄存器分配与内部路径——plain/纯 LTO 构建侥幸残 0；PGO 不同训练
profile 改变 regalloc 与路径 → 一红一绿；同一红态内不同 deopt 走
不同内部路径残值不同 → 命中面随 profile 变动（enum vs test_slice）。
全部存量谜团闭合。**工具链无罪，C3 轮相应判决与 REPORT 风险项撤销。**

**修复**：移除该版本门（一处，两架构同修）。红态原位判决：同一份
红 gcda（仅 generator.cpp 因源变更失谱）重建，复现探针全绿转
（jit-list repro、import 探针、test_slice/exceptions/raise/int；
test_builtin 失败为既有基线已知项，与本案无关，另见配方修正）。

**配方修正**（build_pgo_lto_311.sh）：①相四探针剔除 test_builtin
（JIT 基线已知分歧项，入探针会把绿链恒判红——探针模块必须全部
基线绿）；②历史注释改写为已收口；验收步保留为构建体系常设防线。

**方法论教训**：①"概率性、构建配置相关"不足以定罪工具链——
未定义寄存器残值类缺陷有同样指纹，先以寄存器级证据定界；②gdb
行号断点在 -O3 下落点漂移（本案落进公共 error 路径），条件式含
无原型函数（PyErr_Occurred）会因"unknown return type"每停假中，
真事件断点应锚定唯一路径的机器码地址（本案 error 标签无异常冷块）；
③bool 经寄存器跨 ABI 边界传递时，"从未装配"与"未归一化"都表现为
非 0/1 脏值——审计顺序应为：接收端实参 → 返回端归一化 → 中间
搬运层。

## 五、门禁与交付口径

**修复后垫片实证**：活体反汇编确认 resumeInInterpreter 调用序列新增
`mov x3, x1`（与旧垫片恰差此一条指令），断点实收
`is_instrumentation_deopt=false`。

**三相链采样**：修复后按交付配方独立重训连跑 5 链，**5/5 相四全绿**
（202~218 s/链，五个互异 md5 产物）；对照修复前同配方首链即红。
训练非确定性风险由缺陷级降回常规级，相四验收步保留为常设防线。

**门禁**（第 5 链 PGO 产物）：冒烟七件全过；diffgate 全绿；refcount
六组零漂移；libtest 26 模块仅 test_builtin/test_scope 两既有已知项
（与 T1 轮完全一致，无新增分歧）；3.14 反向 ninja 重建 + 两 smoke
通过（≥3.12 发射序列不变，行为零变化）。

**定向 A/B**（存档 m10pgo-ab0/-ab2-summary.json）：

- auto=0 判别口径（六项）：几何 **0.852**，较 T1 轮纯 LTO+O3 的
  0.834 **+1.8pp**——我方 PGO 兑现，T1 残差从 ~17% 收窄至 ~15%
  （deepcopy 0.761 / pickle 0.779 / pprint 0.873 / go 0.929 /
  unpickle 0.894 / richards 0.892）；
- JIT 态（11 项）：几何 **0.995**（T1 轮 0.979，+1.6pp）；守护组
  全线抬升无回退（richards 2.239 / richards_super 2.198 /
  deltablue 1.308 / raytrace 0.970）；go 0.644（T1 0.668，跨构建
  ±6% 噪声带内）。

**交付口径判决：PGO+LTO 三相恢复为正式性能口径**——工具链障碍
经查证不存在，红态根因为移植层缺陷且已修复；纯 LTO 降级口径
（C3 轮决策）随本轮撤销。
