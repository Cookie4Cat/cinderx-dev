# M10 第二十五轮：store 桩 values 形行内插入与写侧分型

日期：2026-07-09　分支：`dryrun/m10-store-insert`　基线：!71。
诉求：上轮直方图排出的 store 桩插入形（44 万/20 迭代，行内命中率
4.5%）。

## 一、实现

1. **values 形行内插入**（gen_asm.cpp do_store）：原 `old==NULL`
   一律回落使新属性首写全部出线。行内化镜像 helper
   `SplitMutator::setAttr` 插入分支 / stock STORE_ATTR_INSTANCE_VALUE
   的 old==NULL 路径：容量判据同式（size+2<capacity，越界回落
   helper 走通用协议物化）、插入序字节写 values 预头 [-2-新size]、
   值 INCREF 后入槽；无版本戳、无 GC 跟踪对象（values 形无独立字典）。
   物化形插入维持回落——stock STORE_ATTR_WITH_HINT 对插入同样
   DEOPT，系 parity。
2. **写侧 helper 分型计数**（计数模式门控）：values 覆写 / values
   插入 / 物化覆写 / 通用回退四计数入 ICRuntimeStats 并导出。
3. **语义冒烟** smoke_store_insert：乱序插入的物化键序（插入序字节
   的强判据）、删除后重插、自赋值、超容量回落、物化后插入五形态。

## 二、分型揭示：store 面的真实构成

| 构成 | 量/20 迭代 | 处置 |
|---|---|---|
| values 插入 | 6.8 万 | 本轮桩吸收（helper 侧归零） |
| 非 split 类条目（kCombined 等） | ~24 万 | **下一杠杆**：Combined 形覆写桩支线（tp_dictoffset 寻址+hinted 覆写，机器与既有物化块同构、条目 kind 不同） |
| 通用回退（键不在共享键/容量满/物化插入） | 13 万 | 结构性回落 |
| 物化覆写 | 0.6 万 | 既有桩路径 |

helper 进入 46.3 万→39.4 万（−15%）。原"插入形 44 万"的直方图
推测经分型修正：插入仅居其中零头，大头为 Combined 条目——**分型
计数先行的价值即在避免按错误构成投入实现**（本轮实现在计数之前
已完成，幸而 stock-parity 方向不亏）。

## 三、判据（v17，存档 full113-ab-v17-summary.json）

- 同味 plain best-of：sqlalchemy_imperative 中性（带重叠，吸收量小）；
- 全集 v17：86 项零失败，几何 1.035（与 v16 持平）；**创建密集族
  同向兑现：raytrace 1.218→1.245（+2.1%，进前六摆动）、chaos
  1.228→1.244、hexiom 1.145→1.157、float 1.136→1.145**——新对象
  属性初始化正是 values 插入的形态；sqlalchemy_imperative
  1.029（v15/16/17 = 1.030/1.047/1.029 自身噪声带内）；richards
  2.789→2.831 回升，佐证上轮"档值抖动"判定；
- 门禁：PGO 链、冒烟 13 件（含新冒烟）、diffgate 940 案 0 新增、
  libtest 26/46 仅既档项、RCM 六组双模全等（写侧引用 asm 红线）。

## 四、遗留

- **Combined 形覆写桩支线**（24 万/20 迭代）：非 managed-dict 或
  物化出生型接收者的 kCombined 条目，全部经 C helper 的
  CombinedMutator——下一个 store 面杠杆；
- lm 类型方法扫描 21.5 万、kind-7 残余 28 万、kwargs kwnames 恒等
  缓存（见上轮遗留清单）。
