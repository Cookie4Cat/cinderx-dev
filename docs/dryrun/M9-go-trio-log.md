# M9 优化第六轮：go 三件套①②（DescrOrClassVar hint 化 + store 内联 stub）

日期：2026-07-05　分支：`dryrun/m9-go-trio`（基于 refcount-flake 分支）
基线：IsTruthy 轮（richards 15.06ms/1.46x，几何均值 0.859x）

## 一、① DescrOrClassVar 读路径 hint 化

go 每窗口 400 万次条目命中落 `DescrOrClassVarMutator::getAttr` 的
实例字典遮蔽检查（类默认值被实例遮蔽形态），每次付字符串哈希字典
查找（PMP memcmp 之源）。为该 mutator 增设 me_key 自验证 hint，
`ci_peek_instance_attr_311` 重构为带 hint 内核 + 薄包装；物化实例
unicode 键直读（split 包装读 ma_values、combined 读 me_value），
LoadMethodCache 慢路径与实例属性方法位共用同一 hint 槽。

## 二、② store 内联 stub（kStoreAttrCachedFastPath）

写侧全泛型是计数矩阵的既定大项（richards 3.6M/raytrace 6.3M/go
2.3M 次 helper 调用+扫描+分发）。新调用形 LIR 指令按五点清单接线；
aarch64 stub 条目判据与读侧一致，命中后行内完成 values 覆写或物化
hint 覆写（镜像 stock 两种 STORE_ATTR 特化的 old 非空路径，物化
覆写按 PEP 509 以 [P2] 影子发号器戳 ma_version_tag，GC 未跟踪字典
回落）；插入/旧值 refcnt<2（dealloc 路径）/general 键/其余 kind
回落 helper。**写侧条目 kind 恒为 kSplitInline（KnownOffset 晋升
只在读侧 getAttrInline 发生），stub 接受 2/3 两值**——首版只认 3
致 stub 全 miss（计数矩阵当场暴露：sa_stub==sa_invoke）。

## 三、排障史：两个真实缺陷（一潜伏一新引入）

1. **split 包装容量越界（潜伏，波及已合入的 C++ hint 路径）**：
   split 包装字典的 `ma_values` 是实例创建时的原 values 数组，容量
   （预头 [-1] 字节）定格；`ma_keys` 是**其后仍可成长的共享键**——
   hint < dk_nentries 仍可能 ≥ 包装数组容量，越界读写即任意堆访问。
   修复 = 容量守卫加满六处：store stub、load attr stub 物化分支、
   C++ 的 hinted peek/getAttrInlineKnownOffset/setAttr 物化分支/
   LoadMethodCache 实例属性方法位。
2. **old == value 别名 stale 计数（新引入，asm 写序）**：写块把旧
   值 refcount 缓存在 value incref 之前——同一对象重复赋值（
   `o.s = SENT` 循环）时，末笔以陈旧值-1 抹掉刚加的 +1，每次调用
   净 -1 直至归零早释（refcount 变 pymalloc 空闲链指针）。修复 =
   decref 前重读内存。C++ 参照实现因逐笔访存天然免疫——**行内化
   C++ 参照代码时，凡"读-改-写"跨越其它对象访问的，别名场景必须
   重读**。

排障方法论沉淀：
- **macOS 绑定挂载下的陈旧构建陷阱**：一次"values 单独也崩"的二分
  结论实为未重链的旧 .so——此后每次构建以 md5 前后比对确证；
- **C++ 镜像核查**：stub 写块临时替换为等语义 C++ 调用（重演槽位
  推导并核查后执行写），一步区分"输入错/写错/环境错"；
- **受害者看点条件化**：先 rc≤0 抓归零现场（凶手 decref），再
  rc>2^40 抓的只是 pymalloc 回收（尸检）；
- 引用计数平衡冒烟（1 万次同值覆写 + getrefcount 对账）是本案
  唯一在门禁层面前置暴露问题的用例——保留为 store 语义门禁标配。

## 四、量化（进程内稳态，同机同法）

| 基准 | 轮初 | 轮末 | 备注 |
|---|---|---|---|
| richards | 15.06ms | **14.49ms（1.52x）** | store helper 调用 3.7M→518k（86% 行内） |
| deltablue(100) | 1.71ms | **1.66ms（0.97x）** | 逼近 stock |
| raytrace(100²) | 149.7ms | 147.5ms（0.90x） | |
| go | 102.9ms | 112.5ms | stub 扫描叠加在原 helper 路径上的税；③ 未做，专项未完 |

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档 m9trio-ab-summary.json
+ 复测 m9trio-ab2-rerun.json）：**几何均值 0.859x → 0.874x**。
richards 1.490→**1.575x**、pickle 0.650→0.726、sqlglot 两项
0.807/0.769→0.825/0.801、scimark 0.815→0.837、spectral_norm
0.742→0.762；generators/richards_super/raytrace 首跑离群复测回
方差带（0.672/1.542/0.896）；**go 0.578→0.549 可复现小回落**
（全 miss 店面叠加 stub 扫描税——③ 未做，专项延续）。

## 五、门禁

diffgate 923 全绿；generators 语料/基础 libtest 差分基线原样；
refcount 矩阵五组零漂移（确定化判据版）；三轮冒烟叠加复跑全过
（含本案关键的同值覆写引用计数平衡）；3.14 反向编译（ninja）+
attr/method/store/module 冒烟通过。

## 六、遗留

- go 三件套③（编译态劣于解释态的判定/回退策略）未动，go 净值仍
  为负——待 ③ 与 values 守卫分支化重评一并处理；
- store stub 的插入分支（含 values 插入序与物化 ma_used）未行内，
  维持 helper；
- DescrOrClassVar 尚无 stub 行内路径（helper hint 化已消字符串
  哈希，剩余为调用税）。
