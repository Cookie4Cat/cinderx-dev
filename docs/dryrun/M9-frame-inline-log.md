# M9 优化第十一轮：帧仪式行内化（编译期常量折叠）

日期：2026-07-05　分支：`dryrun/m9-frame-inline`　基线：帧协议轴首轮
（几何均值 0.892x；richards 14.32ms / deltablue 1.63 / raytrace
172.9 / go 101.1，进程内同日口径）

## 一、动机与结构

3.11 kNormal 模式每次编译函数调用付**两次出线 C 调用**：入口
`JITRT_AllocateAndLinkInterpreterFrame_Release`（PushFrame 越界检查
+ InitializeSpecials 约十个字段 + localsplus 全置空 + 链接），出口
`JITRT_UnlinkFrame`（解链 + ClearExceptCode 逐槽 XDECREF + code
decref + 弹栈）。而 framesize/nlocalsplus/code/prev_instr 全部是
编译期常量——完全可以像 stock `CALL_PY_EXACT_ARGS` 那样行内化。

**关键发现：行内机器已备而未接**——LWF 模式早有 `FrameInitPlan`
（consteval 字段表 + StorePair 分组发射，localsplus 置零逻辑连注释
都写好了）与 `emitInlineUnlinkLeafFrame/FastFrame`（LIR 分支惯用法
+ 冷区回落），kNormal 只是仍走 C 调用。本轮实质是把这套机器接到
3.11 普通帧语义上。

## 二、实现

1. **`buildFrameInitTable` 参数化**（interpframe.h）：consteval
   `bool lightweight` 双表——`kFrameInitTable`（原语义，旗标选择）
   与新 `kNormalFrameInitTable`（完整解释器字段集：f_globals/
   f_builtins/frame_obj/is_entry/stacktop，镜像
   `_PyFrame_InitializeSpecials`）。`FrameInitPlan::build` 增表参数，
   localsplus 置零逻辑去旗标门改由参数驱动（LWF 调用点显式传 0，
   惰性物化语义不变）。
2. **入口 `emitInlineLinkNormalFrame`**：kLoadThreadState → 数据栈
   碰撞指针分配（top+framesize 与 limit 无符号比较，越界回落 C 全
   路径含新 chunk 分配）→ FrameInitPlan 常量填帧（StorePair 成对
   + localsplus 置零）→ f_func/f_code 各 incref 一次（镜像
   Py_NewRef(func) + InitializeSpecials 的 NewRef(code)）→
   previous/current_frame 链接。
3. **出口 `emitInlineUnlinkNormalFrame`**：三前置检查（检查期零突
   变，任一失败整体回落 `JITRT_UnlinkFrame`）——① frame_obj 已物
   化（逃逸帧归属转移）；② f_locals 已写（`PyEval_GetLocals` 族可
   在无 frame_obj 时直写）；③ 帧位于 chunk 基（弹出须释放 chunk）。
   全空时 cell-free 资格保证 localsplus 自入口起全 NULL，剩余语义
   = 解链（先于清理，GH-99729）+ f_func/f_code 各 decref（makeDecref
   自带 dealloc 分支）+ datastack_top 回拨。
4. **静态资格**（入出口同一谓词）：`co_ncellvars == 0 &&
   co_nfreevars == 0`（否则 InitFrameCellVars 写入 localsplus，出口
   无法静态证明全 NULL）且非调试构建；`CI_JIT_NO_INLINE_FRAME=1`
   为排障/测量开关。cell/free 函数与 3.14 维持原 C 路径。

排障一处：LIR 无条件 Branch 之后误用 `appendBlock` 挂 slow 块——
`appendBlock` 会给当前块追加 fallthrough 后继，已以跳转终结的块
必须用 `switchBlock` 放置纯分支目标（LIR 验证器当场拦截，报
"does not contain a jump to non-immediate successor"）。

## 三、量化（进程内稳态，同日对基线）

| 基准 | 基线 | 行内后 | Δ |
|---|---|---|---|
| richards | 14.32ms | **11.34-11.40** | **-21%（vs stock 22.6 ≈ 2.0x）** |
| deltablue | 1.63ms | **1.51**（stock 1.57） | **-7%，首次反超 stock** |
| raytrace | 172.9ms | **152.0** | **-12%** |
| go | 101.1ms | 96.9 | -4% |
| generators | 27.0-27.7 | 27.1 | 持平（生成器帧不走此路径） |
| unpickle | 5.10 | 5.16 | 带内 |

## 四、过程中检出的两个既存问题（均非本轮引入，已归档/立项）

1. **locals() 函数拒编**（PYJIT_RESULT_UNKNOWN_ERROR）：开关对照
   证实与本轮无关，3.11 前端既存限制（M3 locals 快照缺口同族）；
2. **编译态递归配额减半异常**：进程经历约千次"编译函数内异常被
   捕获"（或两千次 sys._getframe 物化）后，编译路径可达递归深度
   精确变为限额一半（limit=5000 实测 cap=2499），解释路径同进程
   4998 正常；单次触发不复现。开关对照 + 合并基构建复现确证既存，
   已立独立专项（确定性复现脚本 /tmp/rec_repro.py、/tmp/rec_min.py
   在容器内）。本轮新增的帧语义冒烟（逃逸/locals/异常穿透/跨 chunk
   深递归/万次调用引用平衡/traceback 身份）保留为门禁标配。

## 五、门禁

diffgate 923 全绿；refcount 矩阵六组零漂移；既有两轮冒烟 + 新帧语义
冒烟全过；3.14 反向编译（ninja）+ attr/method/store/module 冒烟通过
（interpframe.h 参数化与 LWF 调用点在 3.14/LWF 语义位相同）。libtest
差分 test_scope 与 test_builtin 两项 DIVERGE 与上轮完全一致，均为
既存跟踪项，本轮相对基点无新增。

## 六、A/B（19 基准，auto=2/w3/p3v5，19/19 全绿，存档
m9fi-ab-summary.json）

几何均值 **0.892 → 0.933**（速通战役至今最大单轮增幅）。
**richards 1.582 → 1.944、richards_super 1.614 → 1.926**；
**deltablue 0.977 → 1.107（A/B 口径决定性反超 stock）**；raytrace
0.794 → 0.862、hexiom 0.728 → 0.772、generators 0.754 → 0.779、
go 0.593 → 0.632、chaos 0.960、float 0.934、scimark 0.882；
fannkuch/nbody 带内小落（1.289/1.226）。调用密集组全面上行印证
帧仪式税是此前 <1.0 组的共同分母之一。

## 七、遗留

- cell/free 变量函数仍走 C 路径（可扩展：出口对已知 cell 槽位定数
  decref）；
- 递归配额减半专项、locals() 拒编限制（正式 M3/M6 范围）；
- 帧协议轴下一深水位：LWF 3.11 移植 / speculative inlining 拍板件。
