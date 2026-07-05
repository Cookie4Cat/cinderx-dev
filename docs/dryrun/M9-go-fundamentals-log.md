# M9 优化第九轮：go 基础优化（方法缓存楔死与终身 kSplitInline 归因）

日期：2026-07-05　分支：`dryrun/m9-go-fundamentals`　基线：policy-off 轮
（几何均值 0.845x；go 0.360 为最大净负项，进程内 167.6ms vs stock
61.4ms；无策略体制，全部病灶已暴露）

## 一、证据采集：计数矩阵 + kind 直方图（新增诊断计数器）

新增 `la_hit_kind[8]`（条目命中的 kind 直方图，计数模式导出
`la_hit_kind_0..7`）。go 每统计窗口的两处大项与归因修正：

1. **lm 侧楔死**：`lm_keys_fail` 6,007,890（= 4 条目 × 1,502,015 次
   查找）、`lm_slow` 1,502,015、`lm_fill` 104——组合字典接收者的
   共享键版本判据永不可比，而规范版本未动故不驱逐、槽全占故 fill
   永不成功，每次方法查找付 4 次键版本败 + 全量 `_PyType_Lookup`；
2. **la 侧归因修正**：4,092,678 次条目命中此前记为 kDescrOrClassVar
   ——直方图证实 **100% 是 kind 2（kSplitInline）**，kind 7 流量为
   零。gdb 函数断点在 -O2 内联下从未解析是先前误归因的成因之一
   （直方图计数器自此成为 kind 归因的标准手段）。

## 二、根因：天生物化形态使读侧条目终身 kSplitInline

go 的核心类分步初始化超出共享键容量，实例出生即物化，属性只存在于
自身组合字典的替换键集——`ensureValueOffset` 永远失败（名字不在
类型共享键），条目永不晋升 KnownOffset：

- **C++ helper**：`getAttrInline` 失败分支直落 `PyObject_GetAttr`
  全泛型（每窗口 400 万次字符串哈希查找）；
- **stub**：kind 门只认 kSplitInlineKnownOffset(3)，kind 2 全 miss
  ——与 store stub 首版同一课（写侧条目恒 kind 2）。

## 三、落地两修

1. **lm 遮蔽直判**（`LoadMethodCache::lookup` keys_fail 分支）：带
   hint 的实例遮蔽检查（物化字典 me_key 自验证 + split 包装容量守
   卫），未遮蔽即缓存有效（类侧由条目级 tp_version_tag 钉住）。
   `lm_slow` 1,502,015 → 8；
2. **la kind-2 双收 + 物化 hint 直读**：stub kind 门改
   `sub #2; cmp #1; b.hi slow`（收 2/3），values 路径加 val_offset
   符号守卫（kind 2 未解析 = -1）；`getAttrInline` 失败分支改带
   hint 物化直读。缺失语义论证比 KnownOffset 更强：fill 选择 split
   形态的前提是 `_PyType_Lookup(type, name) == nullptr`（类侧连非
   数据描述符都没有）且版本钉住，通用协议只剩实例字典一步，键缺/
   槽空即 AttributeError。`la_invoke` 4,308,159 → 215,481（残余
   全部是 module 站点命中，另一形态）。

## 四、实现后被证据否决并撤除的两项（存档设计）

1. **attr stub 的 kDescrOrClassVar 遮蔽分支**：三道语义门
   （tp_descr_set 空 / managed dict / 物化形态）后跳共享物化直读块。
   直方图证实 kind 7 流量 go 为零、raytrace 40k 且全为未遮蔽形态
   ——hint 只能证明"名字在键集"，无法行内证明缺席，分支零捕获，
   按证据纪律撤除；
2. **lm stub 首槽探测**：以 name 缓存哈希直探组合字典首槽，
   DKIX_EMPTY 即开放寻址不变式下的缺席证明（删除留 DUMMY 不留
   EMPTY）。实测 100% 命中转化（`lm_stub_probe_hit` 1,502,007、
   `lm_helper` → 8）但计时完全中性——修①后的 helper 已够便宜，
   调用开销不是约束。否定性结论：**lm helper 密度不再是 go 杠杆**。
   设计存档备用（宽索引表/general 键/碰撞回落 helper 的无状态证明）。

## 五、量化（进程内稳态，同机同法）

| 阶段 | go | 备注 |
|---|---|---|
| 轮前（policy-off 终态） | 167.6ms | |
| + 修①（lm 遮蔽直判） | 154.3ms | -8% |
| + 修②（kind-2 双收，同构建对照 161.8） | **104.2ms** | **修② -36%；轮累计 -38%** |

richards 14.6-15.4 / deltablue 1.74-1.83 / raytrace 173.6-179.2，
均在跨构建方差带内。go 残余（~104 vs stock 61）已不在 IC 轴：帧/
调用协议 + 无内联（generators 轮与 speculative inlining 同源）。

## 六、门禁

diffgate 923 全绿；refcount 矩阵五组零漂移；两轮冒烟复跑；3.14
反向编译（ninja）+ attr/method/store/module 冒烟通过（`kindBits()`
首版误置于 <0x030C 门内，3.14 反向编译当场拦截后移出）。

**libtest 差分：test_scope testLeaks 在 JIT 下确定性泄漏一个实例**
——已确证基点构建（d6e27a004，MR #26 后）同样失败，**非本轮引入**
（本轮相对基点无新增分歧）；M2 基线时代该模块通过，引入轮次待二分
归因，已立专项。

## 七、A/B（19 基准，auto=2/w3/p3v5，19/19 全绿，存档
m9gof-ab-summary.json）

几何均值 **0.845 → 0.865**。**go 0.360 → 0.579（本轮标的，单项最大
增幅）**，A/B 与进程内口径（104 vs 61 ≈ 0.59）首次一致——此前
0.36 的额外缺口即楔死的方法缓存与全泛型 la 路径在新进程短窗口下的
放大。richards 1.480 / richards_super 1.526（历史最佳）/ deltablue
0.904 / chaos 0.917 / fannkuch 1.306 / nbody 1.224；其余带内。

## 八、遗留

- go 残余轴：帧/调用协议 + speculative inlining（#29 与内联专项）；
- la module 站点 215k/窗口（stub 无 module kind 短路；量级次要）；
- test_scope 泄漏专项（独立会话二分归因）。
