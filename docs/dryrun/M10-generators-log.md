# M10 第十二轮：生成器专项——恢复仪式瘦身与同步生成器编译策略

日期：2026-07-06/07　分支：`dryrun/m10-gen-round`　基线：
自适应去特化轮（#49 合入后，交付口径 early quicken × auto=4 ×
adaptive despec，全集 v7 几何 0.966）

## 一、对象

全集残余劣化中 generators 0.666 / coroutines 0.801。上轮 B 探针
三态快诊已给出方向性判决（despec 轮志第四节）：同步生成器编译
净效应 −11%（stock 72.9 / 纯解释 86.0 / JIT 96.6 ms），协程相反
+13%（23.4 / 33.9 / 29.9）。本轮任务：定罪慢因、按证据处置。

## 二、探针定罪（普通构建，perf 999Hz + 进程内 best-of）

两条待验假说，一立一破：

1. **钩子每恢复税——洗冤**。假说称 [P3] 帧压栈计数钩子对生成器
   每次恢复重复计税；perf 实测钩子链在编译态生成器负载中占比可
   忽略（热生成器编译后恢复不经解释入口，钩子只在建器边界付费）。
   该假说不成立，不作为修复对象。
2. **JitGen 恢复仪式——定罪**。编译版生成器每次 send/next 的
   固定仪式（`PyIter_Send` 分发 → `jitgen_am_send` 槽调用 →
   `send_core` → footer 恢复入口，含 exc_info 交接与帧链接）
   合计约 25% 周期。生成器体本身（yield 间的用户代码）在
   pyperformance generators 这类微小体形态下占比过低，编译收益
   无从摊薄仪式成本——这就是 B 探针 −11% 净效应的机理面。

## 三、处置一：恢复仪式瘦身（保留，墙钟中性）

实现三层派发合一：`jitgen_am_send` 拆薄壳与实现体
（`jitgenSendImplInternal`），`jitgen_iternext` 与
`JITRT_GenSend` 在槽位仍为规范 am_send 时直入实现体（绕过
PLT/槽间接跳，各省一层出线调用；槽位被 `with_deopt` 换装期间
自动回退慢路径，暂停语义不变）。

同味同构建 stash A/B 判据：修前 94.34 ms / 修后 95.08 ms——
**墙钟中性**。perf 剖面份额 −7pp 但未兑现为墙钟：Apple-M 级
乱序核对预测良好的间接跳近乎免费，成本实体在 send_core 本体
与溅射恢复（与 IC 轮"计数占比不外推为墙钟"同一教训）。改动
保留的理由：弱乱序核（950 目标机）上间接跳成本结构不同，存在
兑现可能；语义零变化、门禁全绿；真机复测后再定去留。

## 四、处置二：同步生成器不自动编译（主交付）

策略依据即 B 探针净效应判决。实现：

- 配置 `compile_sync_generators{false}`（旗标
  jit-compile-sync-generators / PYTHONJITCOMPILESYNCGENERATORS），
  仪式瘦身落地或 950 实测反转后可单开关回退；
- 资格门置于 `ci_autoJit311AllowsCode`（[P3] 计数钩子与
  scheduleJitCompile 双入口共用）：`CO_GENERATOR` 且不含
  `CO_COROUTINE | CO_ASYNC_GENERATOR` 位者不计数不编译——同步
  生成器连每帧计数税一并豁免；
- 协程/异步生成器不受影响（净效应 +13%，且 D6 本就禁编协程
  本体，编译收益来自外围机械）；force_compile 不经此门，
  diffgate/refcount 语料的编译态生成器覆盖面保持。

判据（普通构建，进程内 best-of，ms）：

| 配置 | generators | coroutines |
|---|---|---|
| 策略开（默认） | **83.56**（compiled=162） | 32.56 |
| 旗标开=旧行为 | 95.61（compiled=167） | 32.6 带内 |
| 纯解释参照 | 86.0 | 33.9 |

策略态优于纯解释参照：热生成器回解释器吃 PEP 659 行内特化，
外围机械（建器、驱动循环、非生成器热函数）仍编译——混合执行
两侧红利兼得。

## 五、门禁与全集（v8，B-only 复测，与 v7 同 A 侧参照）

门禁：PGO 交付链相四验收 OK；冒烟七件全过；diffgate 全绿；
refcount 六组零漂移（含 corpus_generators——force_compile 不经
资格门，编译态生成器覆盖面保持）；libtest 仅
test_builtin/test_scope 两既有项（与上轮收口完全一致，
test_scope 修复在旁路会话分支待合入）。

全集 86 项（存档 full113-ab-v8-summary.json，A 侧沿用 v6 参照，
崩溃零）：几何 **0.966 → 0.975（历史新高）**，<0.90 项 22→19，
≥1.0 项 36。

- **generators 0.666 → 1.012**——历史最大劣化项转持平以上，
  兑现幅度超出 B 探针预估（~0.85）：策略态混合执行优于纯解释
  参照，与第四节进程内判据一致；
- coroutines 0.774 精确持平（策略零波及，按设计）；
- 误伤面点名核查全部无伤：html5lib 1.054、tomli_loads 1.148、
  pyflate 1.085、xdsl 0.885（历史带 0.882-0.896 之内）、
  async 族带内摆动（asyncio_tcp_ssl 1.152）；
- genshi 0.932 越出历史带（0.971-1.010）下沿，同构建定向 A/B
  定罪：旗标复原旧行为仍 0.942 vs 默认 0.937（0.5pp，噪声级）
  ——**非本策略所致**，为本次 PGO 重建的跨构建漂移。

## 六、遗留

- send_core 本体与溅射恢复为仪式残余主体，yield 点溢出瘦身
  （路线③）为后续候选；
- 协程编译启用（解除 D6）为独立正确性战役；
- 协程解释路径 vendored vs stock 差距（interp −31%）审计未启；
- 950 真机复测仪式瘦身与本策略（微架构敏感判断以真机为准）。
