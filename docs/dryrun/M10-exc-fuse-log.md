# M10 第十九轮：异常率试用（deepcopy_reduce 二榨）与 #62 泄漏热修

日期：2026-07-08　分支：`dryrun/m10-reduce-squeeze`　基线：!63。
950 回报 #62 后 deepcopy_reduce +5% 兑现但仍差 5%。

## 一、定罪：copy._keep_alive 的每调用异常税

修后重剖析 + DeoptStats：deepcopy_reduce 每次 deepcopy() 在
`copy._keep_alive` 吃一次 KeyError→deopt（`memo[id(memo)]` 首访
必炸、handler 置初值——stdlib 惯用形）；函数体三行，deopt 物化
全额税零摊薄。3.11 异常模型下 handler 必经 deopt 走解释器，编译
该形态纯亏；解释器原生处理同一异常零额外成本。

## 二、机制：异常率试用（exc-rate probation）

三代演进（每代以插桩/对照定界）：

1. **纯计数熔断**（无回边 code 的 UnhandledException deopt 越限
   即冻结）：reduce −7% 但 plain deepcopy **+11% 误伤**——同一
   `_keep_alive` 在 reduce 形异常率 ~50%（冻结净赚）、在普通
   deepcopy 形 ~33%（dict/list/subdict 三次调用一次 KeyError，
   冻结净亏）。**异常率随负载而变，判决必须按率**；
2. **异常率试用 v1**：越限只挂计数包装器，K 次调用内按
   异常/调用比裁决。两缺陷实测定界：转正不粘滞→计数续涨→反复
   再武装→包装态抖动（两基准全面劣化）；K=64 恰可整窗落在混合
   负载的高异常率相内（相位采样误冻）；
3. **终版**：转正经 skey 粘滞位固化（冻结经 ROI FROZEN 天然
   粘滞）；窗口 256 跨相位；冻结阈 40%（两侧实测点 33%/50% 之间）；
   带回边 code 首次越限即置粘滞位永不武装（load 类"一次 deopt 摊
   数千次派发"形态零打扰）。

**性能红线排障**：首版把逐字节码回边扫描放在每次异常 deopt 必经
路（deepcopy 每基准迭代 1800 次 KeyError deopt 全额付扫描+慢速
GetExtra，交错 A/B 实测 +40%）——重排为 co_extra 快读+纯字段
检查在前，扫描仅在武装决策点执行（每 code 至多一次）。包装器侧
extra 获取同改快读（出线版曾伤 pprint/deepcopy +15%）。

## 三、搭车热修：#62 全局装载行内化的双 incref 泄漏

本轮 RCM 抽查曝出 corpus_controlflow 带漂移（with-语句族 Ctx
+200/迭代级），stash 定界为已合并 base（#62）所致——#62/#63 两轮
均未跑 RCM。最小复现（6 行 with 函数）+ HIR dump 铁证：
`Incref v29; Incref v29; … Decref v29`——手工发射的 Incref 与
HIR 引用计数插入 pass 的自动份重复。修复：借用装载的引用账全权
交 pass，删除两形（module/builtins）的手工 Incref。修后最小复现
delta=0、RCM 三组 CLEAN。**教训：①改动 HIR 发射的引用语义必跑
refcount 矩阵（#62 靠冒烟+diffgate 放行，语义对但账错）；②本
管线的所有权由引用计数插入 pass 统一管理，发射端手工 Incref 即
双记账**。

## 四、判据（plain，进程内 best-of，ms）

| 基准 | 修前 | 修后 | 说明 |
|---|---|---|---|
| deepcopy_reduce | 3.25-3.40 | **3.05-3.14（−7%）** | _keep_alive 冻结 |
| deepcopy | 5.38-5.65 | 5.58-5.61 | ON/OFF 交错零差，误伤根除 |
| pprint / unpickle / richards / spectral / coroutines | — | 全部带内 | load 不受扰动 |

行为面双向验证：reduce 主导冻结 ✓、plain 主导转正 ✓（各 N=3）、
load（带回边）永不武装 ✓。门禁：冒烟十件全过、exc_inject_fuzz
64 发 0 崩、diffgate 0 新增（4 转好）、RCM 三组 CLEAN。

## 五、遗留

- 950 建议合并后复测 deepcopy_reduce（预期兑现大于本机——deopt
  物化在弱乱序核更贵）；
- 异常率试用与计时试用（研究旋钮）共用 probation_ctl 状态机,
  两者并开的交互未测（计时试用默认关,交付无此组合）;
- M6 的异常表编译（handler 进产物）是本机制的根治向上位替代。

## 六、崩溃热修追录(MR #65)

950 与预演机全量复现:六项异常惯用形密集基准(argparse×2/
asyncio_tcp/docutils/dulwich_log/sphinx)SIGABRT。旗标二分定罪
本轮机制;根因为包装器"先查编译入口→裁决(可能冻结卸载)→调用
先前查得入口"的时序缺陷——冻结后调用悬垂入口(gdb 下时序改变
即隐身;despec 无此病:其卸载在 deopt 路径,恢复走解释器)。
修复:裁决前移至查找之前,冻结后查找落空自然走解释入口。

红线入档:凡裁决动作可能使既有句柄失效,裁决必须先于句柄获取。
门禁缺口:冒烟/diffgate/RCM 语料异常密度不足以在 256 窗内触发
冻结,策略机制需要异常密集语料(待补)。

修后完整收口(v14,存档 full113-ab-v14-summary.json):零失败,
几何 **1.030 → 1.040(历史新高)**,六项复活(argparse 1.112/
asyncio_tcp 1.064/dulwich_log 0.987/docutils 0.925/sphinx 回带),
richards 2.857/deltablue 1.596/pprint 1.207 续升,≥1.0 共 54,
<0.90 仅 6(忽略族 4 + sphinx 已归因 + gc_collect 已判决——
实项清零);回退侧均为带内摆动。门禁:PGO 相四验收/冒烟十件/
diffgate/refcount 六组全绿,libtest 两既有项。
