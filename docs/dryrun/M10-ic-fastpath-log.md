# M10 第三轮：IC 快路径正面修复（C1 类型接收者缓存 + C2 __getattr__ 放行）

日期：2026-07-06　分支：`dryrun/m10-ab113-fixes` 续　基线：归因轮
（M10-laggard-attribution-log.md 的共因清单与预估）

## 一、实现

**C1-lm　类型接收者委托**（inline_cache.cpp/h）
LoadMethodCache::lookup 遇 `PyType_Check(obj)` 受者，惰性建立内嵌
LoadTypeMethodCache 并委托——type_getattro 语义复刻与 tp_version_tag
拉式校验（D5）全部复用既有实现，净新增仅接线。此前类型受者在通用
条目（按元类型键控）与慢路径（type_getattro 早退拒填）两头落空，
永久慢路径。

**C1-la　类型接收者多条目缓存**（inline_cache.cpp/h）
AttributeCache 增设 8 条目轮转缓存（惰性分配，不涉类型受者的站点
零内存税），两种形态：
- kValue：解析结果与受者 MRO 原始属性同一——纯类变量、自返描述符
  白名单（函数/wrapper/method 描述符，get(attr,NULL,type) 恒自返）、
  staticmethod 解包。受者 + 元类型 tp_version_tag 双拉式校验。
  classmethod 排除（每次访问新建绑定对象，缓存会改变 `is` 语义）；
  自定义描述符即使本次自返亦不收（可能有状态）。
- kMetaDescr：元类型数据描述符（type.__name__ 等 getset）缓存描述
  符本体、命中活调其 get。**按元类型键控**——同名 getset 对该元
  类型的一切类通用，单条目吸收全部多态受者（docutils 数十个节点类
  轮换 type.__name__，按受者键控 8 槽被打穿的实测教训）。元类型
  版本单校验（数据描述符优先级不受受者字典影响）。
取代原单槽 siteExt 的类型分支（多态受者单槽即失效 + 仅纯类变量
两道限制正是 pprint 1440 万次慢路径的成因）。

**C2　slot_tp_getattr_hook 命中侧放行**（inline_cache.cpp）
- 探针：该 typeobject.c 静态函数不导出，首次使用时以
  `class _CixGetattrProbe: def __getattr__(...)` 探针类捕获指针；
  探针失败一切比较落空、行为不变。
- la：慢路径结果**溯源**到通用阶段（实例属性直读同一或 MRO 解析
  同一）才填充；lm：绑定方法 __func__ 与 MRO 解析同一才回填通用
  条目。__getattr__ 兜底产物（代理转发等动态结果）不溯源不缓存。
  填充后命中即通用阶段命中，__getattr__ 不再参与，语义不变；删除/
  遮蔽/类变更经既有共享键版本与 tp_version_tag 校验自然失效回慢
  路径，彼时重走完整 hook 语义。溯源设计同时绕开"数据描述符抛
  AttributeError 须进 __getattr__"的语义陷阱（hook 语义不需线程化
  贯穿慢路径主体）。

## 二、排障实录（红线级，两案）

**案一：计数模式六连崩。** PYTHONJITCOLLECTINLINECACHESTATS=1 下
maybeCollectCacheStats 对 cache_stats_ 无空判直接解引——
initCacheStats 只在 LIR 分配点按旗标配套调用，运行期增设的委托
实例未初始化即崩（语义冒烟未崩纯属分配布局运气）。双修：空防护
（正确性不依赖初始化配对纪律）+ 委托实例继承宿主缓存的统计标识。
**红线：统计/诊断基础设施的初始化契约对运行期增设对象默认不成立，
消费侧必须防御。**

**案二：sphinx 语义回归（A/B 假 719x 现形）。** 终态 A/B 中 sphinx
B 侧 0.6ms/迭代——先排除测量假象（A 侧原始预热逐迭代稳定 0.44s、
B 侧 1.75→0.08→0.0008 塌缩，实为逐迭代崩溃）；三开关一次构建机制
二分（la 类型受者/lm 委托/C2 各配临时禁用变量）定罪 C2；填充日志
显示 `C2FILL Config.language inst` 与 sphinx 异常
`'Config' object has no attribute 'language'` 对上——根因：
**sphinx Config 为"__getattr__ 惰性填充"模式**（属性首访时由
__getattr__ 写入实例字典），C2 据首个实例的溯源填充 split 条目后，
**后续全新 Config 实例的空槽"确定 miss"走 raise_attribute_error
直接抛错，跳过了 __getattr__ 兜底**，每次构建瞬间失败、耗时坍缩。
修复取单一漏斗方案：raise_attribute_error 是全部缓存 kind"确定
miss"的唯一出口，在其内对 hook 类改走完整 PyObject_GetAttr（含
__getattr__ 兜底，镜像 slot_tp_getattr_hook 尾部语义），一处修改
覆盖 split/combined/描述符全部 kind；通用类行为不变。sphinx 预热
恢复稳定 0.54s；该模式固化为回归卫兵（smoke_ic_typerecv 的
LazyCfg 用例：百次全新实例读 + 未知属性仍正确抛错）。
**红线之二：给某类接收者放行缓存时，"命中语义等价"不够，还必须
审"miss 语义等价"——mutator 的确定-miss 快速抛错对 hook 类是
语义分歧；单一漏斗处修一次即全 kind 生效。**

## 三、计数验证（差分口径，修复前 → 后）

- pprint：la_slow **240 万/值 → 0**（type.__repr__ 站点整体吸收，
  la_site_type_hit 240 万/值）；
- docutils：type.__name__ 67 万残留经元类型键控清零后
  la_site_type_hit 78 万 → 101 万/值；la_slow 364 万 → 271 万/值。
  残留榜首为范围外家族（Text.children/paragraph.attributes 实例侧
  fill 拒绝、method.__name__ 方法对象受者、NoneType.document 异常
  控制流）；
- sqlalchemy_imperative：la_slow 15.5 万 → 8.8 万/值（−43%）、
  lm_slow 10.8 万 → 5.7 万/值（−47%）、lm 条目命中 +5 万/值（C2
  回填生效）。**墙钟持平——慢路径量非其瓶颈**，−26% 编译净效应
  另有出处（perf record 专项移交下轮）。

## 四、门禁与量化

门禁：冒烟六件（新增 smoke_ic_typerecv：类型受者/元类型描述符/类
变量失效/类型受者方法/__getattr__ 全语义边界 + LazyCfg 回归卫兵）
全过；diffgate 923 全绿；refcount 六组零漂移；libtest 相对基线无
新增分歧（test_generators 双模同败为环境项，非分歧，另行观察）；
3.14 反向编译 + 两 smoke 通过。终态构建 = PGO 三相配方重跑。

终态 A/B（16 项，存档 m10ic-final-ab-summary.json；对照 = 本轮前
PGO 基线）：

- **pprint 0.772 → 0.842（+7.0pp）**——type(obj).__repr__ 站点
  1440 万次慢路径清零的直接兑现；
- 面上普涨：unpickle 0.798→0.822（+2.4）、sqlglot_v2 0.835→0.847、
  xdsl 0.838→0.849、docutils 0.740→0.750、django_template
  0.840→0.848；
- sqlalchemy_imperative 0.687 持平——慢路径量降 ~45% 而墙钟不动，
  确认其 −26% 编译净效应另有出处（perf record 专项，见第五节）；
- sphinx 0.821：回归修复后回到原位，卫兵在岗；
- 守护组无回退：richards 2.245 / richards_super 2.232 / deltablue
  1.301 / raytrace 0.993（均在跨构建漂移带内）。

计数已清而墙钟未全兑现的部分与归因模型一致：类型受者命中仍付
helper 出线往返（C3 桩扩展范围）+ T1/T2 解释器侧税占大头。

## 五、范围外记账（供下轮取单）

- C3 la 桩 kind-7 扩展（sqlglot 540 万/值 helper 往返；pprint 吸收
  后的 340 万/值 helper 命中同理）；
- 实例侧 fill 拒绝家族（Text.children/paragraph.attributes——
  canCacheAttribute 共享键冲突拒填的物化实例形态）+ sa 存储侧
  （docutils sa_slow 93 万/值）；
- 方法对象受者（method.__name__ 57.9 万/值）；
- sqlalchemy 编译净效应根因（慢路径量已排除，需 perf record）；
- T2 钩子链内联、T1 vendored 本底构建专项。
