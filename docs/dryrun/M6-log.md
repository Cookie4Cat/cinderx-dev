# M6 预演日志：deopt 与异常正确性（递归守卫 / tracing pause / 异常注入 fuzz）

日期：2026-07-04　分支：`dryrun/m6-deopt`　基线：`dryrun-311-base@bc28a19d3`（M5 合入后）

范围（设计书 M6 行 + M5 移交项）：递归深度复核（M5 移交）、forced-deopt
状态恢复冒烟（localsplus/指令位置）、settrace 往返（D8 tracing pause）、
异常注入 fuzz（count-then-inject，JSC exception-fuzz 同款）。
明确不做（正式 M6 范围）：任意点 localsplus 重建、栈上帧强制回退、
C 栈软限第二层。

## 一、递归深度：SIGSEGV → RecursionError（M5 移交项销案）

**基线行为**（`scratch/m6-deopt/recursion_smoke.py`，修复前）：

| 探针 | 结果 |
|---|---|
| A 解释器深递归 | RecursionError ✓ |
| B JIT 自递归（force_compile 后 100000 层） | **SIGSEGV**（C 栈耗尽） |
| C 镜像一致性（JIT 调用+deopt 后解释器探针） | limit=200 时 ~198 层触发 ✓ |

判定：`tstate->recursion_remaining` 本身无漂移（C 探针），缺的只是
JIT 帧的检查——编译代码不经过 `_PyEval_EvalFrameDefault` 入口，绕开了
解释器在该处的递归深度检查。与主线（3.14）已知问题同源：JIT 历史上
两种帧模式均无递归检查。

**预演修复**：编译函数的 vectorcall 槽位安装 C 包装
`recursionGuardedVectorcall`（context.cpp，`< 0x030C` 版本门），入口处
执行与解释器一致的 `_Py_EnterRecursiveCallTstate` 计数与检查。修复后
三探针全绿；kwargs 再入、自递归、去优化回退冒烟均正确。

**核心发现：`func->vectorcall` 的"入口即身份"三重不变量。** 该指针在
kunpeng 架构中同时承担三个角色，安装 C 包装会破坏后两个：

1. 调用入口（包装天然兼容）；
2. **REENTRY/STATIC_ENTRY 偏移基址**：5 处站点按
   `func->vectorcall ± 固定偏移` 再入生成码（jit_rt.cpp 4 处：
   kwargs 绑定、参数数目修正 ×2、静态签名打包；lir/generator.cpp 1 处：
   InvokeStaticFunction 发射期）。包装指针参与偏移运算会跳入垃圾地址。
   处置：新增 `jit::jitVectorcallEntryBase()` 统一解析真实生成码入口，
   五站点全部改经该函数。
3. **编译态判定**：`isJitCompiled()` 以"vectorcall 指针落在 JIT 代码池内"
   为判据。处置：识别守卫包装后回退按 Context `lookupFunc` 判定。

**生产落地建议**：正式开发应将检查做进生成码入口桩（prologue 级）——
入口桩已存在错误出口（参数绑定失败路径），可复用，从根上避开三重
身份问题；vectorcall 包装作为可行性证明保留在预演分支。

**残余缺口（记档）**：① 生成器 resume 不经 vectorcall，深生成器递归
链仍无保护；② C 栈软限：`sys.setrecursionlimit` 调大后 JIT 帧仍消耗
真实 C 栈（3.11 解释器帧内联不消耗），需主线两层设计中的
c_stack_soft_limit 第二层；预演不做。

## 二、deopt 状态恢复 + settrace 往返（S1–S4 冒烟）

`scratch/m6-deopt/deopt_state_smoke.py`，interp/jit 双模式子进程输出
逐行对比；jit 模式在函数执行中途经 `force_uncompile` 触发入口交换。

| 场景 | 结果 | 说明 |
|---|---|---|
| S1 中途 deopt 后 f_locals/f_lineno | **DIFF** | f_lineno 正确（相对行 +3）；**f_locals 为空**。M3 已立案缺口（寄存器 locals 不落 localsplus）在 deopt 点的首次实测定量。任意点重建（复用 reifyLocalsplus + DeoptMetadata）= 正式 M6 主体工作。 |
| S2 异常穿过 deopt 帧 | OK | traceback 行级等价（M5 帧修复 + instr_ptr 物化的直接收益）。 |
| S3 预装 tracer 事件流 | 修复后 OK | 修复前 jit 模式事件流为空（JIT 码静默不产生 trace 事件）。 |
| S4 执行中装 tracer、撤除后恢复 | OK | 撤除后 JIT 正常复用。 |

**S3 修复 = D8 tracing pause 预演版**：递归守卫包装中检查
`tstate->cframe->use_tracing`，激活期间新调用一律改道解释器入口
（解释器自带递归计数，该路径不重复计数）。修复后 call/line/return
事件流与解释器逐项一致。残余：已在栈上的 JIT 帧不产生事件，生产版
需帧级 deopt 或 OSR-out（正式 M6/D8 范围）。

## 三、异常注入 fuzz 原型（count-then-inject）

**运行时开关**（jit_rt.h 协议 / jit_rt.cpp 基建，版本无关、env 未设时
仅一次布尔判断开销）：
- `CI_EXC_INJECT=count`：统计检查点总数，退出时 stderr 输出
  `CI_EXC_INJECT_TOTAL=<n>`；
- `CI_EXC_INJECT=<n>`：第 n 个（1 起）检查点合成
  `RuntimeError: ci-exc-inject #<n>` 并令 helper 走失败返回路径。

**检查点挂法两类**（对应生成码调用运行时的两种形态）：
1. 运行时 helper 直挂：调用族 `JITRT_Call` / `JITRT_Vectorcall` /
   `JITRT_VectorcallPythonFunction` 头部检查（3 站）；
2. **LIR 发射期垫片替换**：LoadAttr/BinaryOp 把 C-API 指针直接烘焙进
   生成码、无 helper 可挂，故发射期按开关替换为
   `excInjectBinary<F>` / `excInjectGetAttr` 垫片（14 项 binop 表 +
   GetAttr；Power 为三元操作维持原实现）。

**扫荡结果**（`scratch/m6-deopt/exc_inject_fuzz.py`，PYTHONHASHSEED=0
确定性复现；判据：注入进程不得出现信号级崩溃）：

| 模块 | 检查点 | 扫描 | 崩溃 | 注入异常可见 |
|---|---|---|---|---|
| corpus_calls | 488 | 全扫 488 | 0 | 488 |
| corpus_controlflow | 58 | 全扫 58 | 0 | 54（4 例被用例自身 except 吞掉，合法） |
| corpus_frames | 35 | 全扫 35 | 0 | 35 |
| corpus_ic_mutation | 56 | 全扫 56 | **3** | 56 |
| corpus_operators | 775 | 全扫 775 | 0 | 775 |
| corpus_unbound | 14 | 全扫 14 | 0 | 13（1 例被吞，合法） |
| corpus_hotloops | **19,290,032** | 抽样 200 | 0 | 200 |

合计 1,626 次注入，3 次崩溃，全部位于 ic_mutation。

**首跑战果：ic_mutation 崩溃案获得确定性复现配方。** 三例崩溃
（检查点 41/42/43）模式一致：注入异常正确终止
`case_method_instance_override` 后，**下一个用例**经共享 helper
`read_method` 段错误——异常中断使属性 IC 残留损坏状态，后续类变异 +
读取踩中失效条目。归因对照实验：`PYTHONJITATTRCACHES=0` 下检查点增至
76（IC 快路径消失、通用 helper 调用变多），全扫 76 点 **0 崩溃**。
确定性复现：`PYTHONHASHSEED=0 CI_EXC_INJECT=41 _bootstrap.py jit corpus
corpus_ic_mutation 0`。此前 M5 记档的 ic_mutation 重复调用 SEGV 复现率
仅 2/3，本配方将 M7 验收语料中该案升级为确定性用例；设计书"异常注入
fuzz 对 JIT 异常路径损坏类问题直接对症"的判断在预演规模上得到验证。

**情报：检查点计数是执行期通过次数而非编译期站点数**——hotloops 单模块
1929 万次通过，全检查点扫荡在热循环模块不可行。生产版扫描策略必须
分层：小模块全扫 + 热模块分层抽样；JSC 的 `at-or-after-N` 旋钮
（fireOSRExitFuzzAtOrAfter）正是为此形态设计，正式 M6 应实现两档语义。

## 四、终验

- diffgate 918 用例：**9 失败 = R3 基线原样（0 新增 0 修复），0 infra
  错误**，两次复跑（递归守卫+身份修复后；注入基建后、env 关闭）；
- refcount 矩阵（calls，interp/jit，N=200）：两模式 0 漂移；
- 3.14 反向回归：ninja 全量 0 错；JIT 冒烟（kwargs/递归/settrace 原状
  ——3.14 走原生成码入口，不经包装）通过。

## 五、移交与工时

- 正式 M6 移交：任意点 localsplus 重建（S1）、帧级 tracing deopt（S3
  残余）、生成器递归保护、C 栈软限第二层、注入 at-or-after 旋钮与
  cinderjit API 化、prologue 级递归检查落地。
- M7 验收语料更新：终态 9 失败（attr 8 + descriptor 1）+ ic_mutation
  崩溃案（新增确定性复现配方 `CI_EXC_INJECT=41`，取代原 2/3 概率的
  重复调用复现）。
- 预演工时：约 2.5 小时。
