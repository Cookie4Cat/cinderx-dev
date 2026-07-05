# M9 优化第十轮：帧/调用协议轴（send 链压层与入口哈希消除）

日期：2026-07-05　分支：`dryrun/m9-frame-protocol`　基线：go 基础轮
（几何均值 0.865x；generators 0.650 / go 0.579 / pickle 0.746 为
协议税代表项）

## 一、证据采集：隔离树 PMP（220 样本/项）

工作区插曲：本轮起点与 test_scope 泄漏排查会话共用 /src 绑定挂载，
对方 bisect checkout 曾污染一次构建与测量（go 66ms 假数、richards
23ms 混代基线）。协调后双方各迁独立克隆（本轮容器内 /root/frame
自建 CMake 树），跨会话共享工作树自此列为红线。

generators 稳态直方图（220 样本）：**send 协议链 ≈34%**——
send_core ~10%、jitgen_am_send ~6%、JITRT_GenSend ~6%、
PyIter_Send ~4.5%、PyThreadState_Get（含 PLT）~4%、
setCurrentFrame ~2%（send_core 内联体归属）；另
**CompilationKey/phmap 哈希 ~2-4%**。go 直方图：帧仪式
（jitFrameInit/_PyFrame_Clear/Unlink/memset）~5%、入口包装
（recursionGuardedVectorcall + CompilationKey）~2.5%。

## 二、落地三修

1. **入口包装每调用哈希消除**（context.cpp）：守卫包装的编译态
   分派此前每次调用付 `lookupFunc` = CompilationKey 构造 + phmap
   查找（CI_JIT_NO_ENTRY_GUARD 注释点名的三项每调用开销之一）。
   改读 `CodeExtra.jit_compiled`——finalize/uncompile 双侧维护的
   (code, globals, builtins) 精确元组缓存（函数创建重挂接路径的
   同一契约）——co_extra 行内直读 + 两次指针比较，任何不符回落
   哈希。3.11 co_extra 数组布局私藏于 codeobject.c，按 vendored
   3.11.6 逐字镜像只读结构（与内联 stub 镜像 dict 预头同一论证）。
2. **jitgen_am_send 线程态**（generators_rt.cpp）：每 send 一次的
   `PyThreadState_Get()` PLT 穿越改 `_PyThreadState_GET()` 内联读。
3. **JITRT_GenSend 直派**（jit_rt.cpp）：行内镜像 stock
   `PyIter_Send` 首分支——受代理方有 am_send 槽即经槽直调，免去
   每次 yield-from/await 迭代的 PLT 与再分发。设计要点：**必须经
   当前槽而非直呼 jitgen_am_send 具名函数**——JIT 暂停期该槽被
   换装为 `jitgen_am_send_with_deopt`，绕过槽位即绕过暂停语义。

## 三、量化（进程内稳态，同日同机对基线复测）

| 基准 | 基线 | 三修后 | Δ |
|---|---|---|---|
| generators | 28.7-29.4ms | **26.95-27.66** | **-6%** |
| richards | 14.75-15.39 | **13.71-14.32** | **-7%** |
| deltablue | 1.77-1.79 | **1.63**（stock 1.57） | **-8%，距 stock 4%** |
| go | 103.8-105.5 | 101.1 | -3% |
| raytrace | 177-184 | 172.9 | -3% |
| unpickle | 5.04-5.28 | 5.10 | 带内 |

哈希消除惠及所有调用密集项（richards/deltablue 主升力）；
generators 三修叠加 -6%，其余 send 链（send_core 本体、生成器
创建/析构、跨层协议结构）属帧协议深水区（LWF/内联轴）。

## 四、门禁

diffgate 923 全绿；refcount 矩阵**六组**零漂移（本轮起加入
corpus_generators 组）；两轮冒烟复跑；3.14 反向编译（ninja）+
attr/method/store/module 冒烟通过。

libtest 差分两项 DIVERGE 均为既存、非本轮引入：
- test_scope testLeaks：已立专项（并行会话已归因至 M9R3 的
  CompiledFunction 强引用有根链，修复分支 dryrun/m9-nested-func-leak
  验证中）；
- test_builtin CRASH:11：SEGV 归因轮既立档的"组合/体量型毒源"
  （ASAN 队列），随二进制布局显隐——本轮基点构建（faa5406f）
  确定性复现，且 /root/frame 构建树提供了稳定复现锚。

## 五、A/B（19 基准，auto=2/w3/p3v5，19/19 全绿，存档
m9fp-ab-summary.json）

几何均值 **0.865 → 0.892**（IC 内联轮以来最大单轮增幅）。
**generators 0.650 → 0.754**（本轮标的）；**deltablue 0.904 →
0.977**（逼平 stock）；richards 1.582 / richards_super 1.614
（双双新高）；raytrace 0.794、chaos 0.944、float 0.898、nbody
1.252、fannkuch 1.331、sqlglot 0.836/0.790 全面向好。go 0.593
带内微升（其残余在内联轴）。regex_compile 0.817→0.758 为单跑
离群（进程内与其余 18 项方向一致，按方差处理）。

## 六、方法论沉淀

- **跨会话共享工作树 = 相互污染**（checkout 翻动 + 绑定挂载直通
  容器构建输入）：多会话并行必须各占独立克隆/构建树；
- **docker cp 保留宿主 mtime**——旧于 .o 的源文件 make 视为最新，
  陈旧构建陷阱的新变体（md5 前后比对红线再次抓获）；
- gdb 批处理脚本 continue 前必须 run（上轮教训固化：缺 run 时
  零输出貌似零命中）；PMP 以 attach 循环（gdb -batch -p + bt 1）
  为可靠形态，async interrupt 在批模式不可用。

## 七、遗留

- send 链残余（send_core 本体 ~10%、生成器创建/析构）与普通函数
  帧仪式行内化（编译期常量折叠 alloc/init/unlink，镜像 stock
  CALL_PY_EXACT_ARGS）：帧协议轴后续轮；
- LWF 3.11 移植（v1.1 独立件）维持待拍板；
- test_builtin ASAN 首批目标（现有稳定复现锚：/root/frame 树
  faa5406f/57dbc4be + PYTHONJITAUTO=24 无范围阀）。
