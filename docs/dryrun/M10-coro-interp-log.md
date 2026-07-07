# M10 第十三轮：协程解释路径审计与调用/生命周期外围税消减

日期：2026-07-07　分支：`dryrun/m10-coro-interp`　基线：生成器
专项轮（!50/!51 合入后，全集 v8 几何 0.975）

## 一、对象

coroutines 0.774 为剩余劣化中最大的单项未解之谜：B 探针三态曾
测得我们的解释态比 stock 慢 31%（stock 23.4 / interp 33.9 ms），
且 D6 本就禁编协程体——这是纯解释器侧的输，不是编译问题。本轮
任务：定罪 31% 去向、按证据修复。

## 二、审计（perf 999Hz，stock vs 解释态，归一化为绝对 ms）

本轮复现三态（plain 构建）：stock 22.5 / interp(auto=0) 29.7 /
JIT(auto=4) 29.7——上轮生成器策略已吃平 JIT 外围收益，全部残差
7.2 ms 在解释侧。基准形态：bm_coroutines 为递归协程
fibonacci(25)，每轮循环创建约 24 万个协程对象，创建/恢复/析构
即热路径。

按符号语义分组的归一化差分（ms）：

| 组 | stock | interp | delta |
|---|---|---|---|
| eval 循环本体 | 12.68 | 12.63 | −0.04 |
| GC track/untrack/del | 0.29 | 1.35 | +1.07 |
| 调用派发(vectorcall) | 0.00 | 0.95 | +0.95 |
| memset/memcpy | 0.57 | 1.49 | +0.92 |
| 析构链(dealloc/finalize) | 0.98 | 1.75 | +0.77 |
| Ci_EvalFrame 包装 | 0.00 | 0.72 | +0.72 |
| 创建(make_gen/alloc) | 0.43 | 0.89 | +0.45 |
| 其余长尾 | — | — | +1.49 |

**核心判决：vendored 解释循环本体与 stock 精确打平；31% 全部在
协程对象生命周期的外围**——跨 .so 边界（PLT 出线、间接跳回）与
JIT 判别绕行对"每 await 建一协程"形态的逐刀放血。三条被定罪的
路径：

1. **泛型 CALL 跨 .so 乒乓**：stock 行内压栈门 `eval_frame ==
   NULL` 在本端口恒假（求值器即 vendored 循环自身），而 3.11 对
   生成器/协程函数调用不做 CALL 特化（specialize.c 一律
   SPEC_FAIL）——每次协程创建都走 PLT `PyObject_Vectorcall`
   （libpython）→ 间接跳回本 .so 默认入口的往返；
2. **析构全量绕行**：cinderx 初始化时全局替换
   `PyGen_Type/PyCoro_Type.tp_dealloc` 为 jitgen_dealloc（服务
   deopt 后 arena 内存回收），纯解释协程每次析构付两轮 JitGen
   判别 + 无效 deopt 调用 + 出口函数版 GC 记账（PyObject_GC_Track/
   UnTrack PLT，stock 为宏内联）+ 虚调用 free-list；
3. **[P3] 钩子每恢复税**：协程每次恢复（yield/await resume）都
   进帧压栈计数钩子，而 D6 下协程体永不可编译——修后实测该项占
   5.45% 周期（auto=4）。

## 三、修复（三处，全部解释器侧）

1. **[P7] 泛型 CALL 被调方感知行内门**（源级补丁，与 [P4] 同
   学说）：被调方 vectorcall 仍为解释器默认入口
   （Ci_StockEntry311）时行内压栈——压栈帧同样在本循环内执行且
   经过 start_frame 的 [P3] 钩子，语义等价；编译入口/包装入口不
   满足判据，经通用路径进 JIT。附带修复：Ci_StockEntry311 赋值
   提升到 auto 分支之外（此前 jit-list/auto=0 形态下 [P4] 特化
   调用与行内门永久失效，即历史"auto=0 解释态偏慢"悬案的归因
   之一）。红利面不限于协程：所有未特化/去特化后的 Python-Python
   调用（温函数递归派发家族）同吃。
2. **析构直通**：jitgen_dealloc 对非 JitGen 对象改为一次 arena
   归属判定（IJitGenFreeList::owns 新增）——非 arena 内存直接
   交还 stock 析构器（libpython 宏内联 + PGO 产物）；arena 内存
   （deopt 后类型还原但存储在池内）保留自定义释放。
3. **[P3] 恢复不计数**：生成器属主帧跳过计数钩子。生成器体的
   调用次数由创建帧（RETURN_GENERATOR 在线程属主帧内执行）记账，
   按恢复计数既失真（一次调用多次恢复）又构成每恢复固定税；
   compile_sync_generators 开启时生成器仍按创建次数正常成熟。

## 四、定向判据（plain 构建，进程内 best-of，ms）

| 阶段 | coroutines | generators |
|---|---|---|
| 修前 | 29.7 | 83.6 |
| [P7]+析构直通 | 29.2（剖面确认 vectorcall/包装层消失） | 82.7 |
| +恢复不计数 | **28.1** | **77.5** |
| stock 参照 | 22.5 | 72.9 |

诚实记录：[P7] 与析构直通合计墙钟仅 −0.5ms，剖面份额如实转移
（本机乱序核对 PLT/间接跳的又一次吸收，与派发合一轮同象）；
恢复不计数为本轮墙钟主贡献（钩子 5.45%→2.27%，恢复远多于创建
的 generators 收益更大 −5ms）。残余 ~5.6ms 已摊薄为长尾（创建
组 PLT 存根、finalize 链已与 stock 打平、钩子创建残余），且本
对照为 plain 构建对 PGO 的 stock，交付 PGO 口径下差距进一步
收窄。语义抽检：diffgate 940 例 0 新增（4 项转好）、冒烟过、
asyncio/生成器/递归差分脚本与 stock 逐位一致。

## 五、门禁与全集（v9，B-only，与 v7/v8 同 A 侧参照）

门禁：PGO 交付链相四验收 OK；冒烟七件全过；diffgate 全绿；
refcount 六组零漂移；libtest 仅 test_builtin/test_scope 两既有项
（与前两轮收口一致）。

全集 86 项（存档 full113-ab-v9-summary.json，崩溃零）：几何
**0.975 → 0.983（历史新高）**，<0.90 项 19→18，≥1.0 项 38。

- **coroutines 0.774 → 0.961（+18.7pp）**——本轮标的，交付 PGO
  口径下兑现幅度远超 plain 判据（三修在 PGO 产物上叠加放大）；
- generators 1.012 → 1.051（恢复不计数的顺带收益）；
- [P7] 红利面如预期普惠温函数递归派发家族（幅度温和）：sphinx
  +2.3pp、gc_collect +1.3pp（越回 0.90 线）、xdsl +1.2pp、
  docutils +1.1pp、pathlib +1.0pp、django_template +0.9pp、
  pprint +0.8pp、mako +0.8pp；networkx 族 +4~8pp；
  async_tree_memoization_tg +5.1pp；
- 回退侧均为带内摆动（asyncio_tcp −2.5pp、sqlglot_v2_parse
  −2.4pp 等）；
- **测量伪影记录**：v9 存档中 regex_v8 0.773（较 v8 −25.6pp）
  为全集跑动中的瞬时干扰——同构建两次定向复测 1.059/1.051，
  均在历史带（v2-v8：1.011-1.046）内，非本轮回退。存档保持
  原值不改，以此条目为准。

## 六、遗留

- 协程解释态残余长尾（创建组 PLT、_PyCoro_GetAwaitableIter）
  边际递减，暂停外科修复；
- 协程编译启用（解除 D6）仍为独立正确性战役；
- [P7] 对温函数递归派发家族（deepcopy/pickle/pprint/docutils）
  的全集效应待 v9 定量；
- 950 真机复测（本轮三修 + 前两轮积压）。
