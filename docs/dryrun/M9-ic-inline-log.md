# M9 优化第一杠杆：3.11 IC 内联快路径（拉式校验版）

日期：2026-07-04　分支：`dryrun/m9-ic-inline`　基线：M9 归因轮判决
（IC 架构 ~33% 时间占比为最大杠杆）

## 一、现状考古的两个决定性发现

1. **3.14 线已有 4 路多态内联 attr stub**（`kLoadAttrCachedFastPath` +
   `emitAarch64LoadAttrInvokeStub`，aarch64 && ≥3.14 门内）：逐条目
   标签型指针比较 → kind 校验 → 行内 values 加载 → 不朽感知 incref，
   miss 尾跳 C++ helper。M7 版本门关掉的"内联快路径"在 3.14 侧就是它
   ——多态站点问题在该设计里已被 4 条目解决。
2. **3.11 的 SplitMutator 是完整占位符**（getAttr=PyObject_GetAttr、
   setAttr=PyObject_SetAttr）——split-dict 实例属性的每次访问都是全
   泛型协议，这是归因轮 PMP 中 GenericGetAttr/_PyType_Lookup 常驻
   热榜的直接来源，也远比"helper vs 内联"的差距更根本。

## 二、实现（三层，全部 <0x030C 门内或双版本共享门）

1. **helper 层（SplitMutator 3.11 真实现）**：values 形态经 managed
   dict 预头 -4 槽直读（`_PyObject_ValuesPointer` 语义；3.11 共享键
   容量固定、不够即整实例物化，val_offset 恒在容量内）；物化实例
   回退通用协议；fill 侧 `inline_values=true` 点亮 kSplitInline 族；
   写侧维持通用协议（值槽插入需 `_PyDictValues_AddToInsertionOrder`
   配套，列为后续项）。
2. **attr stub 扩展到 3.11**：既有 stub 增加逐条目**拉式校验段**
   （VALID 标志位 + tp_version_tag 与条目记录值比较，镜像 M7 的
   `AttributeMutator::matches`；条目槽为内存读、GIL 下无竞态）；
   values 指针取预头 -32 定偏移（比 3.14 的 tp_basicsize 寻址少一次
   加载）；ob_refcnt 为 64 位普通自增（无不朽位；伪不朽单例巨额
   计数自增无害）。
3. **method stub 新建（3.11 先行）**：新 LIR 指令
   `kLoadMethodCachedFastPath`（调用形，x0/x1 返回恰合
   LoadMethodResult ABI，GetSecondOutput 的 kLoadSecondCallResult
   约定天然兼容——postgen 第二结果重写按 Call 同路处理）；stub 逐
   条目镜像 `LoadMethodCache::lookup`：类型指针 + VALID/版本拉校验 +
   共享键版本校验（keys_version==0 直接有效；values 形态比对
   ht_cached_keys->dk_version；物化回落 helper）；命中双 incref 返回
   (callable, self)。3.14 线无既有方法 stub，本条暂 3.11 独享（上游
   候选另行评估）。

## 三、搭车修复：isValidKeysVersion 的 3.11 values 形态漏检

原实现对 3.11 managed dict 实例落到 `_PyObject_GetDictPtr` → values
形态得 NULL 判"有效"——"名字已在共享键、槽位后填"的实例遮蔽方法
形态会返回过期缓存（触发窗口：同类先创建的实例把名字挤进共享键、
后创建实例的容量覆盖该名，再对后者赋值遮蔽）。补 <0x030C 分支：
values 形态比对 ht_cached_keys->dk_version，物化实例经 -3 槽字典
校验。stub 与 helper 同判据。

## 四、量化（richards 稳态 per-iter，同机同法）

| 阶段 | per-iter | vs stock 22ms |
|---|---|---|
| 归因轮基线 | 35.5 ms | 0.62x |
| + helper 层（SplitMutator 真实现） | 33.2 ms | 0.66x |
| + attr stub | 33.6 ms | （单项贡献被 method 主导掩盖） |
| **+ method stub** | **26.1–26.9 ms** | **~0.83x** |

PMP 复核：LoadAttrCache::invoke / LoadMethodCache::lookup 从热榜
消失，_PyType_Lookup 6→1、GenericGetAttr 显著回落。

## 五、门禁

diffgate 主语料 923 全绿；generators 基线原样（2=locals 既定）；
基础 libtest 差分基线原样（2）；refcount 矩阵（calls + ic_mutation）
零漂移；attr 语义冒烟（values/物化/删除/类变异/实例遮蔽）全绿；
3.14 反向编译 + attr/method 冒烟通过（共享文件改动均在门内或对
3.14 行为无变化：attr stub 门放宽为 3.11||3.14 但 3.14 段逐字未动，
method stub 3.11 独享）。

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档
m9-ic-ab-summary.json）：**几何均值 0.729x → 0.794x**。attr/方法
密集组收益显著：deltablue 0.493→0.761、raytrace 0.542→0.769、
richards 0.626→0.877、richards_super 0.724→0.941、hexiom
0.567→0.667、chaos 0.668→0.724、scimark 0.671→0.721；数值/序列化
类持平（非属性瓶颈，符合归因）；generators 0.706→0.680 轻微回落
（量级近噪声，随帧协议轮复核）。

## 六、遗留与后续杠杆

- StoreAttrCache 写侧 stub（本轮读侧优先）；
- LoadMethodCache 条目数 4 与 attr_cache_size 联动调参；
- 帧协议轻量化（归因第 2 项，~16%）与解释入口残余（第 3 项）未动；
- 3.14 线 method stub 移植评估（上游无此优化）。
