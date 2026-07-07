# M10 第七轮：sqlalchemy 编译净效应专项（IC 慢路径线性键扫）

日期：2026-07-07　分支：`dryrun/m10-sqla-neteffect`　基线：PGO 根因
轮（#42 合入后，PGO+LTO 交付口径）

## 一、对象与三态重测

归因轮遗留：sqlalchemy 编译净效应 −26%，C1/C2 修复使慢路径调用量
−45% 而墙钟持平——根因另在。PGO 交付产物上四口径重测（三层税分离）：

| 基准 | auto=0(T1) | 巨阈值(+T2) | JIT(+T3) | T3 编译净效应 |
|---|---|---|---|---|
| sqlalchemy_imperative | 0.924 | 0.917 | 0.674 | **−26.5%** |
| sqlalchemy_declarative | 0.949 | 0.933 | 0.891 | −4.5% |
| go | 0.917 | 0.769 | 0.627 | −18%（T2 亦重） |
| docutils | 0.924 | 0.904 | 0.761 | −16% |

imperative 的 T3 与归因轮原值几乎不动——五轮优化（C1/C2/C3/T1/
PGO）对其全部无效；且其 T1 已收薄至 −7.6%、T2 仅 −0.8%，损失
几乎纯粹是"编译出来的码更慢"。

## 二、归因（perf 双态对比，进程内稳态跑器）

perf record（999 Hz，各 20 s，容器 perf_event_paranoid=-1）：

- **JIT 态 ~18% 周期落在 IC 慢路径的线性键扫机械**：memcmp 5.7%
  + PyUnicode_Compare 4.0%（另 PLT 2.4%）+ getDictKeysIndex 3.4%
  + LoadAttrCache::invoke/peek_hinted 2.2%，调用链
  `LoadAttrCache::invoke → ci_peek_instance_attr_hinted_311 →
  ci_hinted_keys_index_311 → getDictKeysIndex`（hint 失效即全表
  逐条目 PyUnicode_Compare）；另 stock 字典内部静态函数簇 ~12%；
- 解释态同负载的属性访问走 GenericGetAttr+_PyType_Lookup 哈希
  查找，仅 ~6.7%；
- deopt 机械完全不在榜——净效应与异常/deopt 无关；
- 机理：3.14+ 的 getDictKeysIndex 用 _PyDictKeys_StringLookupSplit
  （哈希），3.11 因该符号不导出而以线性扫权宜。sqlalchemy 受者
  多为物化大字典 + 站点多名字轮换，hint 命中率低，每次慢路径
  = O(键数) 次字符串比较。

## 三、修复

`Common/dict.h getDictKeysIndex`：按 CPython 3.11 dictobject.c 的
unicodekeys_lookup_unicode / dictkeys_get_index 复刻 unicode 键表
哈希探测（变宽索引槽 1/2/4/8 字节 + 扰动开放寻址 PERTURB_SHIFT=5
+ 驻留名指针等值快判 + 哈希预判后深比较；DKIX_DUMMY 继续探测、
DKIX_EMPTY 即缺席；非 unicode 键表返回 -1 从不缓存——旧线性扫对
general 形态本就未定义）。hint 机制保留（命中时仍最廉价）。

## 四、判决（进程内 best-of，同构建同负载）

| 构建 | 解释态 | JIT 态 | JIT/解释 |
|---|---|---|---|
| 修前（PGO 交付） | 26.54 ms | 36.99 ms | **+39%** |
| 修后（普通构建） | 27.95 ms | 27.69 ms | **持平（−1%）** |

perf 复核：修后 JIT 态剖面中 memcmp / PyUnicode_Compare /
getDictKeysIndex / 字典内部簇全部消失，无热点超 2.6%。冒烟
（含 M10 家族一删除条目语义，直接覆盖 DKIX_DUMMY 路径）全过。

## 五、门禁与量化（PGO 三相交付产物）

门禁：相四产物验收 OK；冒烟七件全过；diffgate 全绿；refcount 六组
零漂移；libtest 仅 test_builtin/test_scope 两既有已知项，无新增。

定向 A/B（存档 m10sqla-ab0/-ab2-summary.json）：

- auto=0 判别口径（六项）：几何 0.857（PGO 根因轮 0.852，噪声带
  内）——修复不触解释态，符合预期；
- JIT 态 13 项：几何 **1.052**（该组合首次整体越过 stock）。
  逐项对修前（本轮三态表/PGO 根因轮存档）：

| 基准 | 修前 | 修后 | 变化 |
|---|---|---|---|
| sqlalchemy_imperative | 0.674 | **0.953** | +27.9pp |
| sqlalchemy_declarative | 0.891 | **1.064** | +17.3pp，反超 stock |
| go | 0.627 | **1.046** | +41.9pp，反超 stock |
| docutils | 0.761 | 0.869 | +10.8pp |
| pprint / sqlglot_v2 | 0.834/0.836 | 0.840/0.837 | 持平（税在 T1/T2） |
| richards / richards_super / deltablue / raytrace | — | 2.255/2.143/1.289/1.004 | 守护组无回退 |
| deepcopy / pickle / unpickle | — | 0.726/0.792/0.825 | 持平（短命进程 T1 主导） |

**归因轮 C5（go keys_fail 遮蔽扫描 110 万次/值）随本修复销案**——
其机理即同一条 getDictKeysIndex 线性扫。go 从常年 0.63~0.67 一举
反超 stock，编译净效应由 −18% 转正。

**方法论**：C1/C2 轮"调用次数 −45% 而墙钟持平"的正确解读是单次
成本主导——次数减半而单次仍 O(n) 则墙钟不动；计数直方图定位站点、
perf 采样定位周期，两者缺一不可，且墙钟问题以 perf 先行。
