# M10 第二十六轮：kind-7 遮蔽写双层修复——物化损伤根治与 store 桩支线

日期：2026-07-09　分支：`dryrun/m10-store-kind7`　基线：!72。

## 一、构成实测证伪 kCombined 假设

上轮"分型计数先于实现"教训现学现用：先增写侧 kind 直方图
（sa_hit_kind，与 la_hit_kind 同构）。实测 store 面剩余 24 万的
构成：**kCombined = 0**（fill 仅对非 managed-dict 类型选 combined，
纯 Python 类全 managed）；真身为 **kind-7 遮蔽写 18.1 万**——非
数据描述符位点的实例字典写，即读侧 `__get__` 首访缓存惯用形的
写侧孪生（`obj.cached = val`）；另 kind-6 成员写 5.2 万、
kind-5 数据描述符 setter 0.6 万（必须调用）。

## 二、发现并根治一处形态损伤（本轮核心价值）

`DescrOrClassVarMutator::setAttr` 原实现先调 `_PyObject_GetDictPtr`
——其对 values 形实例有**物化副作用**，而 stock GenericSetAttr 同
位点走 StoreInstanceAttribute 保持 split 形态。即：**我们的首次
遮蔽写一直在把接收者主动打成物化形**，损伤该实例后续所有读写的
快路径形态——这正是 kind-7 读侧轮观察到"SQLAlchemy 接收者全是
物化形"的成因之一。

修复：helper 按 stock 次序重写——values 形优先，共享键 hint 自
验证定槽，覆写与插入行内（抽出 `ci_values_slot_store_311` /
`ci_mat_dict_slot_store_311` 两共享函数）；物化形 hinted 覆写
（GC 跟踪保障 + 影子版本戳）；名字不在共享键/超容量回落通用协议
（stock 同点物化，parity）。hint 与读侧共用，写侧同步刷新（store
只写位点自此也维护 hint）。

## 三、store 桩 kind-7 支线

准入：非数据（tp_descr_set==NULL）+ MANAGED_DICT 接收者。values
形按共享键 hint 自验证定槽，覆写与插入均汇入既有共享写块（插入经
values_insert 完成插入序记录）；物化形按字典键 hint 定槽（覆写戳
版本；插入经 x15 非零判定回落）。

## 四、判据（v18，存档 full113-ab-v18-summary.json）

- IC 直方图：sa helper 进入 39.4 万→**20.1 万（−49%）**，kind-7
  18.1 万→7.2 万；**级联效应**——接收者留在 values 形使 kind-2
  命中与通用回退各降逾六成（13.7→5.2 万 / 13.1→4.6 万）；
- 同味 plain best-of：0.02029-0.02095 → **0.01952-0.01991
  （−3.8%，五发全序无重叠）**；
- 全集 v18：86 项零失败，几何 1.034（带内）；
  **sqlalchemy_imperative 1.029→1.074（+4.4%，战役新高）**、
  sqlalchemy_declarative 1.109→1.139、docutils +4.5%、xdsl +6.8%、
  asyncio_tcp +3.7%——描述符缓存惯用形的广谱红利；
- 带外档值复核（同构建单基准重跑）：pathlib 1.013 / hexiom 1.171 /
  richards 2.865 / html5lib 1.122 全部回带，richards 族 ±2% 抖动
  第三次验证；
- 门禁：PGO 链、冒烟 14 件（smoke_store_insert 新增 kind-7 遮蔽写
  段：描述符本体不扰动/删除遮蔽复现/物化后覆写）、diffgate 0 新增、
  libtest 26/46 仅既档、RCM 六组双模全等。

## 五、遗留

- kind-6 成员写 5.2 万（`__slots__` T_OBJECT_EX 槽交换，可行内，
  量小）；
- kind-7 残余 7.2 万（hint 首访/交替接收者/插入形）；
- 读侧 la kind-7 残余 27.4 万（helper 内 hinted 命中，含描述符
  真调用）；lm 扫描 21.5 万；kwargs kwnames 缓存。
