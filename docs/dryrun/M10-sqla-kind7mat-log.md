# M10 第二十四轮：sqlalchemy_imperative 二榨——kind-7 桩物化形准入

日期：2026-07-09　分支：`dryrun/m10-sqla-kind7mat`　基线：!70。
诉求：继续榨取 sqlalchemy_imperative（本机全集口径 1.027-1.030，
950 已达标）。

## 一、取证链

1. **退优化面清零**：DeoptStats 10 次基准迭代 0 deopt——#63 轮证伪
   的"异常 deopts"旧账确证死亡，残余损失不在 deopt。
2. **稳态 perf**（暖机后 attach，避开编译器采样污染——短驱动整程
   采样会被 LinearScan 家族淹没）：IC 各 C helper 命中侧合计 ~6%
   （LoadAttrCache::invoke 1.55 / lookupHelper 1.09 / hinted peek 族
   2.2 / StoreAttrCache 0.62 / getDictKeysIndex 0.78），另
   JITRT_BindKeywordArgs 1.40、解释残留 5%、incICStat 1.24。
3. **IC 计数直方图定罪**（PYTHONJITCOLLECTINLINECACHESTATS，20 次
   迭代）：la 桩入口 144 万而 helper 进入 93 万，其中 **kind-7 命中
   69 万**——占 helper 流量 74%；store 桩行内命中率仅 4.5%（44 万
   全走 C）；lm 扫描命中 21.5 万。
4. **形态读解**：SQLAlchemy 事件派发（PoolEventsDispatch 等）为
   "非数据描述符 `__get__` + 首访缓存进实例 `__dict__`"惯用形——
   稳态全部是**物化字典遮蔽命中**，每次经两个未内联 C 函数
   （hinted peek + keys index）绕行返回缓存值。桩的 kind-7 支线
   （C3 轮）原判据"无 getter"整型排除此形，且接收者限定 values 形。

## 二、实现：kind-7 支线两处放宽（gen_asm.cpp）

1. **准入闸**：`tp_descr_get==NULL` → `tp_descr_set==NULL`（非数据）。
   非数据形态实例字典优先，遮蔽命中行内直返；"确定无遮蔽"的三个
   结论点（无实例字典 / keys_version 快形 / 探测空槽）统一汇至新
   结论点重查 `tp_descr_get`——纯类变量直返 descr，带 getter（绑定
   方法等）出线经 helper 调用 `__get__`，数据描述符维持准入即回落
   （helper 完整定序语义）。
2. **物化接收者支线**：-3 槽字典带 hint 遮蔽探测，镜像 kind-2 物化
   块与 helper peek 物化分支——me_key 指针自验证（hint 失效回落
   helper 刷新）、split 包装容量守卫（共享键成长后 hint 可越界）、
   combined 形读 me_value；值空即未遮蔽转结论点。

语义等价论证：与 `DescrOrClassVarMutator::getAttr` 逐分支对照——
数据描述符（setter 非空）回落；遮蔽值借引 INCREF 与 helper peek
同构；"键在但值空/包装越界"等未遮蔽结论与 helper 的 nullptr 返回
同判；general 键回落。引用语义无新增（返回路径 INCREF 惯例不变）。

## 三、判据

- IC 直方图（同法复测）：helper 进入 93 万→**52 万（−44%）**，
  kind-7 出线 69 万→**28 万（−59%）**；
- 同味 plain stash A/B（进程内 best-of ×5，s）：旧
  0.02074-0.02109 vs 新 **0.02007-0.02065，best −3.2%**，五发全序
  无重叠；
- 门禁：IC 语义冒烟五件先行全过；全套（PGO 链 + 冒烟 12 + diffgate
  + libtest 26/46 + RCM 六组 + full113 v16）随收口链，数字见下节。

## 四、收口（v16，存档 full113-ab-v16-summary.json）

- 全集 86 项零失败，几何 **1.035**（v15 1.038，带内）；
- 目标基准：sqlalchemy_imperative 1.030→**1.047（+1.7pp）**，
  sqlalchemy_declarative 1.107→1.116；广谱红利：django_template
  1.066→**1.092（+2.6pp）**；
- 带外档值复核（同构建单基准重跑）：richards 档值 2.789 复测
  **2.894**、deltablue 1.573 复测 **1.616**，均回带内偏上，
  richards_super 反向（2.625/2.598）佐证——v16 档值下挫为采样
  抖动，非"结论点 getter 重查"机制税，防御性微调不做；
- 劣化面：<0.90 七项 = 忽略族 4 + sphinx 既归因 + gc_collect 既判
  + telco 0.882（历史噪声带内，v15 0.926/v14 0.905 摆动）；
- 门禁：PGO 链、冒烟 12 件、diffgate 940 案 0 新增（4 转好）、
  libtest 46 模块 0 新增、RCM 六组双模全等、libtest 26 模块唯
  test_scope 既档项（scratch 侧基线漂移使其显示为"new"，仓内
  baselines/cp311-libtest-basic.json 口径下属既档，与本轮无关）。

## 五、遗留（本轮直方图揭示的后续杠杆，按流量排序）

- **store 桩插入形**（44 万/20 迭代，行内命中率 4.5%）：每迭代新建
  对象的首写=插入，涉 resize 不可简单行内；可评估"插入后 hint 预热"
  或按 stock STORE_ATTR 特化家族对齐；
- **lm 扫描命中**（21.5 万）：类型方法扫描仍在 C，可评估 lm 桩扩容
  或 kind 化；
- **kind-7 残余出线**（28 万）：hint 首访刷新 + 交替接收者对单 hint
  槽的抖动 + 真描述符调用（不可行内）；
- **JITRT_BindKeywordArgs**（1.4%）：kwnames 为调用方 LOAD_CONST
  元组，指针恒等稳定——可做 (kwnames identity → 槽位重排) 单条目
  缓存挂 CodeExtra；
- incICStat 1.24% 剖面占比与其"旗标关闭仅剩分支"的实现不符，疑
  LTO/PGO 符号归属涂抹，修后剖面复查再定。
