# M10 第十六轮：产物侧调用直派（VectorCall/CallMethod 行内选径）

日期：2026-07-08　分支：`dryrun/m10-call-inline`　基线：LGB 轮
（!54 合入后，全集 v11 几何 1.020）

## 一、对象

deepcopy 验尸清单头号项：泛型调用协议合奏约 7.7% 周期——编译
产物的每个动态调用都付 C helper 夹层（JITRT_Vectorcall/JITRT_Call
→ _PyObject_VectorcallTstate → 间接跳），而解释器同位点是
CALL_PY_EXACT_ARGS 行内压栈。本轮把调用目标选径下沉到产物行内。

## 二、设计：无分支目标选径（csel 选地址 + 单次装载）

发现既有 lowering 已分层：被调方 HIR 类型 ≤TFunc 走薄壳
JITRT_VectorcallPythonFunction，但动态派发形态（从字典/属性装载
的被调方）类型皆为 TObject，全部落厚壳。而 helper 与 vectorcall
槽签名完全同形——可在调用位点行内选径：

- **快臂**：被调方为 PyFunction（类型精确比较）→ 直读其
  vectorcall 槽（被调方入口自带递归检查与 eval-breaker 语义）；
- **慢臂**：其他可调用体 → 经慢路径槽装载 JITRT_Vectorcall/
  JITRT_Call（保留 C 可调用体的周期检查与回落协议）；
- 快慢两臂统一为"从地址装载目标"：快臂地址 = &callable->
  vectorcall，慢臂地址 = 全局槽（值恒为 helper）——非函数对象
  不做越界字段读，csel 选地址免块分裂；
- TFunc 已知者免类型检查直读槽；exc-inject 启用时保留 helper
  发射维持注入位点覆盖面（编译期选径，env 进程稳定）。

kCallMethod 额外处理 JITRT_Call 的 3.11 双 NULL 约定：callable
为 NULL/None（kw 调用原始形）先经哨兵对象（Py_True）csel 替换
免空指针类型读，其类型必非 PyFunction 自然落慢臂；**receiver 槽
（args[0]）为 NULL 表示"无接收者须丢弃首槽"**（LoadMethodResult
规范形，simplifier 预算漏网时由 helper 兜底）——直臂不做移位，
receiver 为空必须落慢臂。

## 三、排障实录：双 NULL 约定之二的教训

首版 kCallMethod 快路径只判了 callable 的 NULL/None，
smoke_laggards/smoke_frame_inline 确定性 SIGSEGV（gdb：
JITRT_CallWithIncorrectArgcount 内再派发到垃圾地址）。二分定罪
（禁用 CallMethod 臂即绿）后重读 helper 全文才发现约定二：
receiver 槽为 NULL 时 helper 要丢弃首槽，直臂带着 NULL 首槽直调
使被调方收到幻影参数、参数窗整体错位。**教训：给 helper 做行内
快路径，必须先通读 helper 全部分支——"签名同形"不等于"语义
同形"，每个 helper 内分支都是快路径的一个准入条件**（与 IC 轮
"桩镜像 helper 必须连形态门一起镜像"同一律）。

## 四、判据（plain 同味对照，进程内 best-of，ms）

| 基准 | 修前 | 修后 | 增益 |
|---|---|---|---|
| richards | 49.8-49.9 | **42.0-42.6** | **−16%** |
| deepcopy | 5.97-6.08 | **5.30-5.39** | **−11%** |
| pprint | 7.85-8.20 | **7.21-7.65** | **−8%** |
| spectral/nbody/coroutines | — | 86.2/75.1/27.7 | 带内向好 |

kVectorCall 臂单独时 deepcopy −6% 而 richards/pprint 持平；
kCallMethod 臂补齐后方法密集形态（richards 类）全面解锁——
覆盖面决定兑现面。语义面：冒烟八件全过、diffgate 940 例 0 新增
（4 转好）、九类可调用体×异常传播×kw 调用差分脚本逐位一致。

## 五、门禁与全集（v12，B-only，与 v9-v11 同 A 侧参照）

门禁：PGO 交付链相四验收 OK；冒烟八件全过；diffgate 全绿；
refcount 六组零漂移；libtest 仅 test_builtin/test_scope 两既有项。

全集 86 项（存档 full113-ab-v12-summary.json，崩溃零）：几何
**1.020 → 1.027**，≥1.0 项 48→**52**，<0.90 项 7 持平（名单
不变）。赢家组全面拉升：richards 2.481→**2.695（+21.4pp）**、
richards_super →**2.500**、deltablue →**1.538**、spectral_norm
→**1.130**、raytrace →1.208、tomli_loads →1.291、hexiom/
html5lib →1.11+、sqlalchemy_imperative →1.033；回退侧
（argparse −4.1pp、unpickle −3.9pp、coroutines −2.2pp 等）均为
带内摆动。

## 六、遗留

- 默认参数补齐（JITRT_CallWithIncorrectArgcount）仍为 helper——
  被调方未知时无法编译期折叠，M6 可评估按 IC 式站点缓存；
- CheckFunctionResult 的 PLT 出线残余（libpython 内部支付部分）；
- CallEx/其他调用形未覆盖（占比小）；
- 950 真机复测积压（六轮）。
