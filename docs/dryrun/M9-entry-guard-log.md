# M9 第十四轮：入口守卫包装消解（递归/tracing 检查下沉编译序言）

日期：2026-07-05　分支：`dryrun/m9-entry-guard`　基线：PGO/LTO 轮
（几何均值 0.964x）

## 一、定价（现成 CI_JIT_NO_ENTRY_GUARD 开关，PGO 构建同日）

裸编译入口对守卫包装：richards **-10.5%**（10.22→9.15ms）、
deltablue **-7.6%**、raytrace -3.5%、go -2.2%——其余优化做完后，
包装层（递归 Enter/Leave + tracing 检查 + 每调用一层 C 帧 + 入口
缓存读）成了最后一块普适每调用税，占比远超见证轮时代的 ~5% 估计。

## 二、设计：零失败路径簿记的账本方案

关键难点是递归账本在全部退出路径上的配平。定型方案：

1. **vectorcall 入口预检不落账**（每函数发射约 9 指令）：tracing
   激活或递归余量将溢（remaining ≤ 0）时，整调用**尾转 stock
   解释器入口**（x0-x3 仍处原始 vectorcall 形态、未压栈）——
   解释器自带 Enter/CheckRecursiveCall 语义（余量/抛错），编译侧
   预检只读不写，失败路径零簿记。分流块置于绑参重入桩之前
   （重入桩到入口的字节距离是 JITRT_CALL_REENTRY_OFFSET 硬不变量
   不可插入其间）；重入路径按设计跳过预检（外层调用已检）。
2. **建帧处扣减**（stock 顺序：帧入链后计数）：绑参失败路径从未
   到达建帧，天然无账。
3. **补账三点**：EpilogueEnd（正常/异常返回）；prepareForDeopt
   （deopt 出口绕过尾声，按 CodeRuntime 旗标补一）；OSR 入口对冲
   扣减（OSR 进入未经建帧扣减而退出走尾声补账）。
4. **资格谓词**（LIR 生成期判定，CodeRuntime 旗标为唯一真源，
   入口发射/finalize 直装/deopt 补账三方同源）：非生成器（初始
   调用经 Yield 机制退出，退出面不闭合，维持包装）、非静态类型
   参数、策略层（ROI/密度/试用）关闭、非 CI_JIT_NO_ENTRY_GUARD。
   生成器与策略场景维持既有包装。

tstate 取法：本目标（manylinux 剥符号静态 Python）的 TLS 偏移探测
被安全禁用（tstate_offset 恒 -1，tls.cpp 有安全注释），改为烘焙
`&_PyRuntime.gilstate.tstate_current` 绝对地址直读（与
PyThreadState_GET 同源，GIL 下即当前线程态；eval_breaker 地址烘焙
同型先例）。

## 三、排障实录：两个"谓词从未生效"级陷阱

1. **tstate_offset 恒 -1**：首版谓词含 `tstate_offset != -1`——
   全员静默回落包装，三口径测量"行内≈包装"当场暴露（若只测
   行内口径会误判收益为零）。
2. **use_tracing 是 uint8_t**：32 位 ldr 带进 3 字节相邻 padding
   垃圾使分流恒真——deltablue/raytrace 全员被打回解释器
   （+28%/+14%），richards 的 padding 恰零而无恙。取证链：三口径
   复测锁定可复现→PMP 见解释器占比 7.5%→稳态断
   `_PyFunction_Vectorcall` 逮到已编译热方法 38/40→排除递归账本
   （现场 remaining=995 健康）与 deopt（三口径计数相同）→入口
   反汇编逐指令核对→断点现场读出 use_tracing 位置 32 位值
   0x4C480000（低字节 0=未 tracing、高位垃圾）。修复=ldrb +
   static_assert 宽度钉死。**红线：镜像 C 结构体字段的行内 asm，
   字段宽度必须 static_assert 钉死——padding 垃圾使误读"按分配
   内容随机显形"，症状可以整基准消失或整基准爆发。**

## 四、量化（进程内稳态，同一二进制三口径对照）

| 基准 | 包装（ROI 旋钮参照） | 守卫行内（新默认） | 裸上限 |
|---|---|---|---|
| richards | 11.59ms | **9.91-10.02（-14%）** | 9.66 |
| deltablue | 1.44ms | **1.31（-9%）** | 1.33 |
| raytrace | — | 145.4-146.3 | 144.9 |
| go | 99.1ms | **94.2** | 96.2 |

守卫行内基本贴住裸上限（差距 ~0-3% = 预检 9 指令），同时保全递归
深度与 tracing 语义（定向冒烟：深递归 RecursionError 正点、3000
异常 + 12000 绑参重入 + 绑参失败 + 多态 deopt + settrace 会话后
解释探针深度稳定 997、settrace 事件从编译函数正常发出）。

## 五、门禁

diffgate 923 全绿；refcount 矩阵六组零漂移；四套冒烟（laggards/
ic_round/frame_inline/entry_guard 新增）全过；3.14 反向编译（ninja）
+ attr/method/store/module 冒烟通过；libtest 两项既存 DIVERGE
（test_scope/test_builtin）无新增。终态构建 = PGO 三相配方重跑
（161 gcda，b1257ab6）。

## 六、A/B（19 基准，PGO+LTO 终态构建，19/19 全绿，存档
m9eg-ab-summary.json）

几何均值 **0.964 → 0.989（+2.5pp）**。richards **2.359** /
richards_super 2.231 / deltablue **1.283** 三项新高；raytrace
**0.969**、chaos 0.987、float 0.980 逼近持平线；scimark 0.908、
spectral_norm 0.871、hexiom 0.816、generators 0.804、go 0.676、
sqlglot 0.850/0.811 全面上行。速通战役几何均值轨迹自此:
0.432 → … → 0.964 → **0.989**。

## 七、遗留

- x86 未发射（谓词按架构关断，维持包装）；
- 生成器族维持包装（初始调用 Yield 退出面）；
- 入口预检 9 指令中 mov imm64 可换文字池载入（-1 指令，边际）。
