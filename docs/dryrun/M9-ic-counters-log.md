# M9 优化第三轮：IC 快慢路径计数器矩阵与三项证据驱动修复

日期：2026-07-04　分支：`dryrun/m9-ic-stats`　基线：见证门控轮
（richards 20.1ms / 1.09x，几何均值 0.808x）

方法论（用户指定）：先给 IC 各层铺快慢路径计数器，拿到 miss 去向
直方图后按证据实现。计数全部挂 `PYTHONJITCOLLECTINLINECACHESTATS`
既有旗标，经 `cinderjit.get_and_clear_inline_cache_stats()` 新增
"globals" 段导出（读出即清零）；两个 aarch64 stub 的入口总数计数
指令仅在计数模式下发射，非计数模式零开销。

## 一、计数器矩阵

- **stub 层**：la/lm stub 入口总数（快路径命中数 = 入口 − helper
  进入数）；
- **helper 层**：LoadAttrCache（invoke/条目命中/split values 命中/
  物化回退/慢路径）、LoadMethodCache（helper 进入/扫描命中/版本拉
  检失败/键版本失败/慢路径/回填）、StoreAttrCache（invoke/条目命
  中/慢路径）、JITRT_LoadAttrInstanceValueOrGeneric（命中/泛型）；
- **站点归属**：la 慢路径按"接收者类型.属性名"记直方图（la_slow_sites）。

## 二、直方图三大发现（richards/deltablue/raytrace/hexiom 稳态窗口）

1. **方法 stub 自诞生即死代码**：`lm_stub_entries` 四基准恒 0，
   richards 每窗口 4,697,120 次方法查找全部直呼 C++ helper，且
   99.99% 在 helper 条目扫描中命中——本应是 stub 内联命中。根因
   `postalloc.cpp` 调用重写：仅 `isLoadAttrCachedFastPath()` 保留
   自身操作码，`kLoadMethodCachedFastPath` 被改写为普通 `kCall`，
   translate 因此直呼函数操作数（lookupHelper），stub 标签从未被
   引用（IC 内联轮的"接线清单"漏了第三处——操作码保留条件）。
2. **store 侧全泛型**：richards 3.6M / raytrace 6.3M 次 STORE_ATTR
   全部 helper 调用 + 条目扫描 + `PyObject_SetAttr` 完整协议
   （3.11 SplitMutator::setAttr 是占位符，IC 内联轮已知遗留）。
3. **la 慢路径两族高频站点**：raytrace 467,284/467,317 次是
   `module.sqrt`（math.sqrt 模块属性）；deltablue 145,365 次是
   `type.FORWARD` 等纯类变量。机制：3.11 全局加载的值非编译期常量
   （dk_version 拉式守卫 + 活值读取），接收者 HIR 类型不可知 →
   simplify 的 LoadModuleAttrCached / LoadTypeAttrCache 分支永不
   触发 → 落到 LoadAttrCache，而 fill 因 `tp_getattro` 非泛型拒绝
   填充 → **每次访问全泛型协议**。

## 三、三项修复

1. **postalloc 操作码保留**（1 行）：`kLoadMethodCachedFastPath`
   与 attr 变体同等保留。richards lm_helper 4.7M → 174。
2. **SplitMutator::setAttr 3.11 真实现**：覆写分支 = stock
   `STORE_ATTR_INSTANCE_VALUE` 的 old≠NULL 路径（values 形态无独立
   字典对象与版本号，直接槽位替换）；插入分支镜像其 old==NULL 路径
   （值写入 + 插入序记录；`_PyDictValues_AddToInsertionOrder` 为
   内部符号不可链接，按 vendored 3.11.6 预头布局逐字复刻：[-1]=
   容量、[-2]=已插入数、[-2-size]=插入序字节；容量判据与 stock
   assert 相同，越界回退通用协议）；物化实例回退通用协议。有效性
   前提同读侧：matches() 已做 tp_version_tag 拉式校验，3.11 共享键
   容量固定故 val_offset 恒在实例 values 容量内。
3. **AttributeCache 站点扩展**（<0x030C 门内，单槽，置于柔性数组
   entries_ 之前）：承接 fill 拒绝的两类接收者。kModule = 值借引用
   + 模块 dict 版本（Ci_DictVersionTag）拉式验证；kTypeAttr = 无
   `__get__` 的纯类变量借引用 + tp_version_tag（VALID 标志）拉式
   验证（MRO 任意层变更经 PyType_Modified 传播失效）。填充判据带
   同一性安全网：仅当泛型协议返回值与 dict/MRO 原对象同一才缓存
   （排除 `__getattr__` 钩子、元类数据描述符、classmethod 等变换
   形态）。借引用安全性：两个版本号发号器全局单调不复用，容器亡后
   地址复用时指针相等而版本必不相等，版本校验通过前不解引用（D9）。

## 四、量化（稳态同机同法）

| 项目 | 见证轮 | +stub 接线 | +store 快路径 | +插入分支+站点扩展 |
|---|---|---|---|---|
| richards per-iter | 20.14ms | 18.79ms | 16.53ms | **16.30ms（1.35x）** |
| deltablue(100) | — | — | 1.89ms | 1.88ms（stock 1.61，0.86x） |
| raytrace(100²) | — | — | 178.6ms | 160.9ms（stock 132.8，0.83x） |

计数复核：deltablue la_slow 145,365→0（la_site_type_hit 全接管）；
raytrace la_slow 467,317→17（la_site_module_hit 467,292）；richards
lm_helper 4.7M→174。

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档 m9ic2-ab-summary.json
+ 复测 m9ic2-ab3-rerun.json）：**几何均值 0.808x → 0.838x**。
richards 1.114→**1.425x**、richards_super 1.168→**1.385x**、
deltablue 0.746→0.883、hexiom 0.658→0.722（收复见证轮回落并反超
IC 轮水位）、raytrace 0.758→0.809、unpickle 0.718→0.750、chaos
0.731→0.761、float 0.869→0.894；数值/序列化类持平。go/generators
首跑走低（0.612/0.628）复测为 0.690/0.722（均高于两轮前值，噪声
判定，两侧方差本就大）。

## 五、门禁

diffgate 923 全绿；generators 语料 2 失败与基线一致；基础 libtest
差分 2 分歧 0 新增；refcount 矩阵五组 interp/jit 零漂移（含
ic_mutation——store 侧引用会计的直接考官）；定向冒烟通过：store
覆写/首插/删除后重插/物化回退/覆写引用计数平衡（10k 次零漂移+
置 None 归还）、模块属性命中/变异失效/恢复、类属性命中/MRO 上层
变异传播/本类遮蔽/删除回退、classmethod 拒缓存语义；3.14 反向编译
（ninja）+ attr/method/store/module 冒烟通过（stub 计数发射为共享
代码但受旗标门控，其余改动均在 <0x030C 门内）。

## 六、遗留与后续杠杆

- **store 侧 aarch64 内联 stub**：本轮 store 仍付 helper 调用 +
  条目扫描（快路径在 C++ 内）；镜像 attr stub 发射覆写/插入分支可
  再收一层（richards 3.6M/raytrace 6.3M 次调用税）；
- 站点扩展仅 helper 层，attr stub 未识别模块/类接收者（miss 后
  helper 命中，已远优于泛型，但仍付 stub 扫描 + C 调用）；
- deltablue lm_slow 13,585（方法扫描 miss → 慢路径）待归属（疑
  多态度 >4 或非函数描述符）；
- IsTruthy TBool 未内联（PMP 榜首，非 IC 范畴）与调用协议税照旧；
- 计数器矩阵为常驻基建：后续轮次直接
  `PYTHONJITCOLLECTINLINECACHESTATS=1` 复采。
