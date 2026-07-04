# M9 预演日志：性能水位摸底（stock 3.11 vs 3.11 + cinderx）

日期：2026-07-04　分支：`dryrun/m9-perf`　基线：`dryrun-311-base@5c5bd67d9`（M8 合入后）

口径：参照 3.14 既有测试协议（auto-JIT 阈值 2、pyperf warmup 3）；
预演严谨度 `--processes 3 --values 5`（正式口径为 pyperformance 默认
约 20 进程）。环境为 manylinux aarch64 容器（Apple Silicon 宿主），
非 D7 锚定环境，绝对值仅供相对比较。

## 一、前置工作：auto-JIT 引导接线（待办清单老项）

3.11 无 function/code watcher，函数创建时无法回调安装 JIT 入口，
`PYTHONJITAUTO` 原先完全不生效。接线设计（预演版）：

1. **求值器惰性安装**（Interpreter/3.11/interpreter.c `Ci_EvalFrame`）：
   函数首次以解释方式进入求值器时，经 pyjit.cpp 桥接
   `Ci_MaybeInstallAutoJitEntry311` 安装编译入口。前提已验证：vendored
   3.11 的 CALL 特化（CALL_PY_EXACT_ARGS 等）在自定义 eval_frame 下
   DEOPT 走通用调用路径，所有 Python 调用都会经过 vectorcall。
2. **预演语义 = 首次解释执行后、第二次调用即编译**（安装
   forcedJitVectorcall），等效 PYTHONJITAUTO=2。不采用 jitVectorcall
   计数入口的原因：其阈值计数依赖 CodeExtra，而 CodeExtra 在 3.14 经
   code watcher 于代码对象创建时分配——**按需分配 CodeExtra 与任意
   阈值支持属正式 M9 接线范围**。

## 二、拦路案：有机 deopt-resume 在全量表面崩溃（本轮最重要发现）

auto 模式首次把 JIT 暴露给完整真实表面（importlib/stdlib），立即
暴露差分语料从未覆盖的路径：**编译函数发生有机 guard 失败 deopt →
resumeInInterpreter → 恢复帧损坏 → SEGV**。证据链：

- frozen importlib（`_find_and_load` 族）与 stdlib enum
  （`EnumType.__new__` 族）两个独立现场；
- gdb 检查恢复帧：函数名/f_globals/f_func 均有效，
  **`prev_instr == -1`（无效值，从未被写入）**——deopt 元数据在该类
  形态下未产出有效指令位。
- 与语料 jit_deopt 模式的差别：checkpoint 强制去优化在**调用边界**
  恢复（函数下次进入时走解释器），而有机 deopt 在**任意字节码位 +
  活操作数栈**恢复——后者只被语料中"corpus 形状"的 guard 覆盖，
  enum/importlib 的形态（类构建期属性 IC 失效风暴）踩中未覆盖分支。

处置：崩溃案立案移交（正式 M6"任意点恢复"主体的直接输入，建议列为
ASAN 后续第一批客户）；预演加范围阀
`CI_JIT_AUTO_ONLY_PREFIX`（冒号分隔路径前缀，双站点共享判定：求值器
惰性安装 + schedule_existing 调度），基准测量口径 = 仅编译
site-packages（工作负载所在）+ 各基准声明的 stdlib 例外文件，冻结/
合成文件名一律排除。

## 三、水位数字

**首个决定性数据点（richards，工作负载 11 函数全部编译）：**

| 配置 | richards 均值 |
|---|---|
| stock 3.11 | ~22 ms |
| +cinderx JIT（递归守卫入口） | ~43 ms（0.51x，**慢一倍**） |
| +cinderx JIT（裸编译入口，CI_JIT_NO_ENTRY_GUARD=1） | ~42 ms（守卫税仅 ~4%） |

**归因初判**：守卫包装的每调用哈希查找非主税。主税为结构性三项
（精确配比需 helper heatmap，正式 M9 首项工具）：

1. **物化帧仪式**：每次 JIT 调用经 helper 分配/清零/链接完整
   `_PyInterpreterFrame` 并在出口解链，而 stock 3.11 特化解释器帧
   压栈为内联数据栈指针递增；
2. **M7 helper 化 IC**：3.11 属性/方法缓存命中全部为 C++ helper 调用
   + 版本校验，而 stock 3.11 有 LOAD_ATTR_INSTANCE_VALUE 级内联特化，
   JIT 反而把特化解释器的快路径换成了通用慢路径；
3. **通用调用路径**：JIT→JIT 调用经 JITRT_Vectorcall/入口仪式，
   stock 特化解释器 CALL_PY_EXACT_ARGS 为内联帧压栈零 vectorcall。

全量 19 基准 A/B 结果：

| 基准 | ratio（stock/jit，>1 为 JIT 更快） | 分类 |
|---|---|---|
| nbody | **1.235x** | **唯一转正**（float 拆箱路径收益） |
| fannkuch | 0.989x | 持平 |
| nqueens | 0.910x | 结构税 |
| float | 0.884x | 结构税 |
| regex_compile | 0.803x | 结构税（热点在 stdlib re，仅部分编译） |
| spectral_norm | 0.741x | 结构税 |
| generators | 0.642x | 结构税 |
| richards_super | 0.593x | 结构税 |
| chaos | 0.537x | 结构税 |
| richards | 0.517x | 结构税 |
| hexiom | 0.130x | **病理级，未归因立案** |
| scimark | 0.067x | **病理级，未归因立案** |
| go | 0.034x | **病理级，未归因立案**（探针显示工作负载未被编译，减速与编译无关） |
| deltablue | B 侧 SEGV | **正确性失败**（疑有机 deopt 家族） |
| raytrace | B 侧 exit 1 | **正确性失败，待归因** |
| sqlglot_v2_parse / transpile | B 侧 TypeError: 'dict_itemiterator' object is not callable | **正确性失败**——真实代码面的执行语义错误，差分语料未覆盖形态 |
| *pickle_pure_python ×2 | 双侧参数错 | 架子问题（本版 bm_pickle 无 pure_python 变体，另行补齐） |

13 项可比基准几何均值 **0.432x**。

## 四、判决与正式 M9 的含义

预演口径下 3.11 JIT 整体处于**净减速区间**（几何均值 0.432x），
且分布为三层：结构税层（0.5–0.99x，call/attr 密集为主）、病理层
（go/scimark/hexiom 8–30 倍减速，未归因，其中 go 的减速与编译无关）、
正确性层（deltablue SEGV、raytrace、sqlglot TypeError——auto 模式在
真实代码面再次抓出差分语料未覆盖的执行语义错误）。唯一转正点 nbody
1.235x 证明拆箱路径有真实收益，架构并非全面失效。
这与"性能可以慢慢优化"的前提冲突——当前差距不在打磨区间，属结构性：
v1 形态（materialized frame + 拉式 helper IC + 无内联）在 3.11 上
输给的是 stock 自己的特化解释器（3.11 的 PEP 659 自适应特化相当能打，
而我们的求值器接管还使其 CALL 特化全数退化）。正式 M9 之前必须先做
的不是"热点收敛"，而是三项结构性回收（对应设计书"不翻 LWF"约束需
重新评估）：

1. 有机 deopt-resume 修复（不修则 auto 模式无法全量开启）；
2. IC 内联快路径恢复（M7 移交项：codegen 级版本比较）；
3. 帧开销：轻量化或 D4 的 LWF 翻转提前评估——richards 类基准若无
   帧+调用两项回收，水位无法转正。

CodeExtra 按需分配（真阈值 auto）、guard 包装 prologue 化随行。

## 五、资产与工时

- `scratch/m9-perf/run_ab.py`（A/B 驱动，pyperf JSON 对比 + 几何均值，
  存档 docs/dryrun/）；范围阀与 CI_JIT_NO_ENTRY_GUARD 开关入库；
- 病理层与正确性层的归因是正式 M9 开工前置（helper heatmap 首个
  客户）；sqlglot TypeError 与 deltablue SEGV 建议纳入差分语料新形态。
- 预演工时：约 3.5 小时（其中 auto 接线与崩溃定位约占三分之二）。
