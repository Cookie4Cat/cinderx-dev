# M9 优化第二杠杆：LOAD_ATTR 精确类型投机的站点见证门控

日期：2026-07-04　分支：`dryrun/m9-frame-light`　基线：IC 内联轮
（richards 26.1ms，几何均值 0.794x）

## 一、诊断链（自顶向下五步闭环）

1. **稳态解释入口直方图**（gdb 断点 + 哨兵 arm）：richards 最热的两个
   谓词方法 `TaskState.isTaskHoldingOrWaiting`（1748 次）与
   `isWaitingWithPacket`（1060 次）在稳态期持续以解释方式进入——
   而 perf map 证明二者**已被编译**（编译产物存在但从不被调用）。
2. **deopt 计数**：稳态期 deopt 为 0——排除"正在反复 deopt"。
3. **ROI backoff A/B**：`CINDERX_AUTOJIT_ROI_BACKOFF=0` 时 richards
   从 25.7ms 恶化到 37.9ms——backoff 在止血：热身期 deopt 风暴触发
   预算耗尽，函数被卸载并把 vectorcall 永久钉回解释入口；稳态零
   deopt、全解释执行、编译产物空转三个现象由此统一。
4. **deopt 原因直方图**（backoff 关闭 + prepareForDeopt 断点）：
   GuardType 289/300，归属函数正是上述谓词本身。
5. **最终 HIR 验尸**：`GuardType<ObjectUser[TaskState:Exact]>` ——
   以**方法定义类**做精确类型守卫。richards 的谓词定义在 `TaskState`
   而实例全部是 `DeviceTask`/`HandlerTask`/`IdleTask`/`WorkerTask`
   等子类，TypeExact 守卫对继承方法**恒假**，每次执行必 deopt。

## 二、根因

`emitLoadAttr` 的 3.11 段存在两处无证据投机（e31387ad5 自
m1-full-port 取料引入）：

- `case LOAD_ATTR_INSTANCE_VALUE`：以 `preloader_.methodOwnerType()`
  （即函数对象所在 `tp_dict` 的定义类）对接收者发
  `GuardType<TypeExact>`；
- switch 后兜底：任何 LOAD_ATTR 变体，只要接收者是 `self`
  （localsplus[0]）且定义类存在，同样发精确守卫。

定义类不构成接收者类型证据：继承方法的接收者是子类实例时守卫恒假；
`LOAD_ATTR_WITH_HINT` 站点（已物化实例）即便类型命中，下游 Simplify
的 split-dict values 形态投机也必然踩 `dict values check` 守卫
（本轮 deopt 直方图中占 11/300 的次要项，同源）。

## 三、修复：站点见证（site witness）门控

投机仅在有站点见证时发射：本站点 3.11 解释器特化缓存
（`_PyAttrCache.version`）实测的接收者类型版本必须等于定义类的
`tp_version_tag`。重构后：

- 见证成立（`LOAD_ATTR_INSTANCE_VALUE` 且 version==owner 版本）：
  在 `tryEmitLoadAttrInstanceValue311` 内发
  `GuardType<TypeExact>` + 通用 `LoadAttr`，由 Simplify 内联
  split-dict 直读——保留单态站点的最快路径；
- 无见证：走既有 `JITRT_LoadAttrInstanceValueOrGeneric` 帮助函数
  （version≠0）或通用 `LoadAttr` → 4 路内联 attr stub（多态容忍、
  无 deopt）；
- 删除 switch case 与兜底两处无证据守卫（受 ROI backoff 保护的
  病理形态不再需要它们兜底）。

LOAD_METHOD 的 `tryEmitLoadMethodWithValues311` 无需改动：其发射
条件本就要求缓存记录的接收者版本与解析出的 owner 版本相等（构造上
自带见证）。

## 四、量化（richards 稳态 per-iter，同机同法）

| 阶段 | per-iter | vs stock 22ms |
|---|---|---|
| IC 内联轮基线 | 25.7 ms | 0.86x |
| **+ 见证门控** | **20.1 ms** | **1.09x（首次越过 stock）** |
| （backoff 关闭对照） | 20.1 ms | 与开启无差别，风暴已消 |

机制指标：稳态 deopt 0；整个稳态窗口解释进入仅 7 次（外层驱动与
线程退出，谓词条目归零）。修复后 PMP 显示 JIT 生成码全面接管热函数；
新榜首为 PyObject_IsTruthy helper 化（~11%）、方法缓存残余 miss、
STORE_ATTR 通用协议、调用协议税——均为下轮候选。

## 五、门禁

diffgate 主语料 923 全绿；generators 语料 23 例 2 失败与基线一致
（locals 既定）；基础 libtest 差分 26 模块 2 分歧 0 新增；refcount
矩阵五组（calls/operators/hotloops/frames/ic_mutation）interp vs
jit 零漂移；定向冒烟（继承多态收敛路径 + 见证单态守卫路径 + 物化/
类变异后语义与引用计数）通过；3.14 反向编译（ninja）+ attr/method
冒烟通过（改动全部位于 <0x030C 门内，3.14 生成代码不变）。

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档
m9wit-ab-summary.json + 复测 m9wit-ab2-rerun.json）：**几何均值
0.794x → 0.808x**。richards 0.877→1.114、richards_super
0.941→1.168（首两项越过 stock）；scimark 0.721→0.750、chaos
0.724→0.731；deltablue 0.761→0.744 与 hexiom 0.667→0.657 为**可
复现的 1-2% 回落**：旧盲目守卫在"接收者恰为定义类"的单态站点上曾
意外获益（auto=2 下解释器缓存未特化、version==0 无见证，新逻辑回
退泛型路径）；raytrace/generators 复测确认为噪声。sqlglot 两项
首跑缺包（环境项，从 dryrun-m1 补 sqlglot 后复测 0.818/0.786）。

## 六、遗留与后续杠杆（按新 PMP 排序）

- IsTruthy 对 TBool 未内联为指针比较，退化为 PyObject_IsTrue 调用
  （新榜首，~11%）；
- LoadMethodCache 残余 miss（多态度超过 4 条目或物化接收者回落
  helper，伴随 _PyType_Lookup/InternInPlace/strncmp）；
- STORE_ATTR 写侧仍为通用协议（PyObject_SetAttr +
  GenericSetAttrWithDict，~6%）；
- 调用协议税（recursionGuardedVectorcall/normalize_datastack/
  allocate_and_link/memset，~12%）——穿刺 d4380b72 的帧/调用协议
  考古继续；
- **无见证单态站点的收益回收**（deltablue/hexiom 回落的对症解）：
  穿刺的 `tryEmitLoadAttrInstanceValue311` 是分支式内联快路径——
  实测接收者 `tp_version_tag` 与缓存版本比较后 CondBranch 内联
  values 直读 / 回退通用路径，**无 GuardType、无 deopt**，多态与
  冷缓存站点天然安全；以定义类版本为比较常量的变体可在编译期解析
  val_offset（`ht_cached_keys` 名字查找），不依赖解释器缓存热度；
- `acc += 1` 整型常量累加器在 3.11 前端仍拒编（既有限制，本轮
  矩阵驱动函数绕行）。
