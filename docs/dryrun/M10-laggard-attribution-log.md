# M10 第二轮：非生成器劣化组正面归因（三层税模型 + 五个结构性共因）

日期：2026-07-06　对象：劣化 >20% 组中除 generators/coroutines 外的
9 项 + 边界带 3 项。构建 = M10 修复轮终态 PGO（md5 ec832807）。

## 一、方法与数据源

1. **三态计时**：stock / auto=0（vendored 循环 + 近零钩子）/
   巨阈值（vendored + 钩子满额）/ auto=2（JIT），run_ab_full113
   协议（p3/w3/v5）；
2. **稳态差分计数**：同基准 n=1 与 n=6 两点差分扣除 import 期噪声，
   IC 全局计数器 + la 慢站点直方图（接收者类型.属性名）+ lm miss
   原因聚合（PYTHONJITCOLLECTINLINECACHESTATS）；
3. **PMP 墙钟采样**：gdb attach 循环 60 帧（docutils/
   sqlalchemy_imperative，JIT 态）；
4. **源码机理走查**：canCacheAttribute / lookupSlowPath /
   la 桩 kind 门 / P3 钩子全链。

## 二、三层税模型（核心产出）

损失 = T1 vendored 本底税 + T2 auto-JIT 钩子税 + T3 编译净效应。

| 基准 | T1 本底(auto0) | T2 钩子(满额) | T3 编译净效应 | JIT 终值 |
|---|---|---|---|---|
| unpickle_pure_python | 10% | **22%** | **+8%（有益）** | 0.798 |
| go | 8% | **17%** | **−19%（致害）** | 0.661 |
| pprint | 15% | 11% | ≈0 | 0.772 |
| deepcopy | **24%** | 5% | +3% | 0.748 |
| pickle_pure_python | **23%** | 6.5% | ≈0 | 0.723 |
| sqlglot_v2 | 14% | 6% | +3% | 0.835 |
| django_template | 14% | 3% | ≈0 | 0.840 |
| sqlalchemy_imperative | —* | —* | **−26%（0.938→0.681）** | 0.681 |
| docutils | —* | —* | **−18%（0.902→0.740）** | 0.740 |
| sphinx | —* | —* | −7% | 0.826 |

（* 三态中仅测了纯解释与 JIT 两态；其纯解释态已接近 stock，损失
几乎全在编译净效应。）

判读：

- **T1（vendored 本底，8~24%）**：钩子近零情况下 vendored 循环仍
  落后 stock，重灾区全是递归/调用密集形态。候选构成：vendored
  求值循环编入 _cinderx.so 的构建质量（我方 PGO 训练集偏基准负载
  vs manylinux 官方深度 PGO）、每帧压栈处的钩子调用位点本身、
  P4/P5 补丁残差。
- **T2（钩子税，满额 5~22%）**：巨阈值下每帧压栈付全链（co_extra
  读 + 前缀过滤 + 计数递增）。真实 auto=2 下热函数编译离开解释
  入口、拒编函数走"已决"快速返回，实付为部分值——但 unpickle 型
  （巨量解释态压栈）满额税 22% 说明该链条在解释态占比负载上是
  一等公民成本。
- **T3（编译净效应）**：unpickle/sqlglot/deepcopy 为正——**编译
  本身并不亏**；go/sqlalchemy/docutils/sphinx 为负——全部对应
  下述 IC 慢路径共因。

## 三、五个结构性共因（C1–C5，均有源码机理 + 站点计数双证）

**C1　类型接收者缓存双拒填**（最大单杠杆）
机理：受者为类型对象时，la 侧 `canCacheAttribute` 对非堆元类型
直接 false（普通类的元类型 = `type`），永不填充；lm 侧
`lookupSlowPath` 首分支 `tp_getattro != GenericGetAttr`
（type_getattro）早退不填。两侧全部永久慢路径。
证据：pprint `type.__repr__` 慢站点 **1440 万次**（la_slow/invoke
= 71%，即 pprint 编译净效应被它吃平）；docutils `type.__name__`
220 万 + lm `type.__init__/__new__` 24 万（WrongTpGetAttro）；
django `type.__subclasses__`；go/deepcopy 的 EnumType 站点族。
修复候选：类型接收者专用缓存——受者自身 tp_version_tag +
元类型版本双拉式校验（type_getattro 语义可复刻：元类型数据描述符
→ 受者 MRO 字典 → 元类型非数据）；ltm/ltac 机器已在，缺动态
受者接线。

**C2　`__getattr__` 类接收者拒填**
机理：类定义 `__getattr__` 后 tp_getattro = slot_tp_getattr_hook ≠
GenericGetAttr → 同上早退，命中也不缓存。但 hook 语义 = 先
GenericGetAttr、AttributeError 才进 `__getattr__`——**命中侧缓存
语义不变，可安全放行**（miss 侧走全路径）。
证据：sqlalchemy `_ConnectionFairy.cursor` / `ConnectionEvents
Dispatch._for_instance/_for_class/__class__` 各 3.3 万（miss 原因
WrongTpGetAttro）+ 慢站点 `_ConnectionFairy.connection` 17.9 万；
sqlglot `_Expression.string/sql_names` 族。sqlalchemy 编译净效应
−26% 的主体。
修复候选：lookupSlowPath/fill 放行 slot_tp_getattr_hook 受者的
命中缓存。

**C3　la 桩 kind-7 拒收 → helper 往返**
机理：aarch64 la 内联桩 kind 门只收 2/3（`sub kind-2; cmp 1;
b.hi slow`），kDescrOrClassVar(7) 命中也要出线进 C helper。
证据：sqlglot_v2 稳态 la_invoke **540 万/值**、慢仅 8.6 万——
kind-7 命中为主但全付 helper 调用；docutils kind-7 命中 96 万/值；
pprint 80 万/值。
修复候选：桩内联 kind-7 命中形态（描述符直返 + values/hint 实例
遮蔽窥视），预计消除百万级/值的 C 调用往返。

**C4　实例属性方法位（ia_）helper-only**
机理：绑定方法存实例属性（`self.read = file.read` 型）走 lm 的
ia_ 活读路径——设计上逐次 helper，桩无此路径。
证据：unpickle lm helper **230 万/值**，其中 ia 命中 ≈100 万
（helper−slow）、慢 128 万；_Unpickler 的 read/readline/append
族即此形态。
修复候选：lm 桩扩展 ia hint 路径（me_key 自验证读已有成熟形态）。

**C5　go 物化键版本败→遮蔽扫描税**
机理：组合字典受者键版本永不可比（go 案已知），mitigation 使
lookup 每次付 hint 定位 + 遮蔽判。
证据：go 稳态 lm keys_fail **110 万/值**（scan_hit 同量）。
修复候选：条目记录物化键版本直比，或桩内联该判定。

PMP 佐证：两代表 60 帧采样中 IC/绑参 helper 族（AttributeMutator/
ci_hinted_keys_index/getDictKeysIndex/JITRT_BindKeywordArgs/
PyUnicode_Compare-in-lookup）持续现身 C 叶帧 ~12-13%，与计数矩阵
量级一致；其余大头在编译码本体（含桩序列与调用协议开销）。

## 四、修复优先级建议（按聚合触达 × 工程确定性）

1. **C1 类型接收者缓存**——pprint/docutils/django/go/deepcopy 全
   命中，pprint 单站点 1440 万；预估 pprint +8~10pp、docutils
   +5pp、其余 +1~3pp；
2. **C2 `__getattr__` 放行**——sqlalchemy −26% 编译净效应的主体，
   预估 sqlalchemy +10~15pp、sqlglot +2pp；
3. **C3 桩 kind-7 扩展**——sqlglot 540 万/值往返，预估 sqlglot
   +3~5pp、docutils/pprint +2~4pp；
4. **C4 ia_ 桩路径**——unpickle +3~5pp；
5. **T2 钩子链内联**（ceval 调用点先读 extra 判"已决"再出线）——
   解释态占比高的全组受益 1~5pp；
6. **T1 本底税专项**（独立轮）：vendored 编译单元旗标/训练面审计
   （computed-goto、-O 级、PGO 训练含长解释负载），潜在全组 5pp+，
   属构建工程而非 JIT 逻辑。
7. C5 go 扫描税——+2~4pp，与 C1 的 EnumType 站点合并处理。

遗留：sphinx 的 −7% 未细分（形态 = 温函数海 + import 重，预计
C1/C2/C3 部分覆盖）；pickle/deepcopy 的 T1 占比最高，指望第 6 条。

## 五、方法论沉淀

- 三态计时（stock/auto0/巨阈值/JIT）一次性把"构建税/钩子税/编译
  净效应"三层解耦，比单一 A/B 的信息量高一个量级，应固化为劣化
  归因标准动作；
- n1/n6 两点差分扣 import 噪声后，短基准的稳态计数才可信（首轮
  矩阵 go/deepcopy 被 import 期淹没）；
- 站点直方图（接收者类型.属性名）是从"慢路径占比"到"根因"的关键
  一跳——本轮五个共因全部由站点榜首直接指认。
