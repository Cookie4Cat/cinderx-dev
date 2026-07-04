# M9 优化第四轮：落后组证据采集与物化形态五案

日期：2026-07-04　分支：`dryrun/m9-laggards`　基线：IC 计数轮
（几何均值 0.838x；落后组 pickle_pure_python 0.626 / go 0.690 /
generators 0.722——此前从未剖析）

## 一、证据采集（计数矩阵 + PMP + 形态验尸）

- **go**：la stub 命中仅 27.6%，59.4 万次条目命中后仍落全泛型
  `PyObject_GetAttr`（物化实例）；store 47 万次同病。ctypes 预头验尸：
  **Square 实例天生物化**——分阶段初始化（__init__ 之外继续添属性）
  超出首实例 values 容量，stock 亦然（解释器以 *_WITH_HINT 行内扛住）。
  次生发现：方法缓存 150 万次全 slow 且 fill 仅 104 次成功。
- **unpickle**：方法查找 104,169 次全部落慢路径、fill 仅 3——
  `self.read`/`self.readline` 是**实例属性中的绑定方法**，类型侧
  fill 永不适用，每次付全量 `_PyType_Lookup` + 实例字典查找。
  （另records采集器两坑：pickle.py 在 stdlib 不在范围阀内；须屏蔽
  `_pickle` 才落纯 Python 实现。）
- **generators**：IC 计数近零——瓶颈在生成器协议本身
  （JITRT_GenSend/jitgen 恢复链 ~20%），归帧/调用协议轴，本轮不动。

## 二、五项修复

1. **带 hint 的物化字典直读（读侧，C++ + stub 行内）**：镜像 stock
   `*_WITH_HINT` 设计——`me_key` 指针比较自验证，hint 任意值均安全
   （越界或键不符即重算并回写 `SplitMutator::mat_hint`）。split 包装
   形态（物化时 `new_dict` 复用共享键与 values 数组）读 `ma_values`，
   combined 形态读 `me_value`。aarch64 attr stub 新增行内物化分支；
   **坑：3.11 `dk_log2_index_bytes` 是索引表总字节数的 log2**（见
   `DK_UNICODE_ENTRIES` 宏），非每索引字节数——首版错乘表长致
   me_key 读偏，单步定位后修正（微例 10 万次访问 helper 进入 2 次）。
2. **物化覆写快路径（写侧，C++）**：`SplitMutator::setAttr` 物化
   分支，镜像 stock `STORE_ATTR_WITH_HINT` 的 old 非空路径（GC 跟踪
   保障 + PEP 509 版本戳走 [P2] 影子发号器）。插入回退通用协议。
3. **实例属性方法位（LoadMethodCache 新 site 槽）**：命中前提 =
   类型指针 + VALID + tp_version_tag 拉式（类侧新增数据描述符经版本
   失效），值经 hint 从实例 values/字典逐次活读（借引用不驻留，删除
   即自然未命中）。unpickle lm 慢路径 104k → 170。
4. **慢路径去物化**：`_PyObject_GetDictPtr` 在 3.11 对 values 形态
   实例有**物化副作用**——方法慢路径（新类型/实例首访问必经）曾把
   工作负载的新生实例批量转入慢形态。新增无副作用直读
   `ci_peek_instance_attr_311`，替换 `LoadMethodCache::lookupSlowPath`
   与 `DescrOrClassVarMutator::getAttr` 两处调用。
5. **共享键成长驱逐**：实例属性跨方法分批添加的初始化模式下，共享键
   每插入新名字 dk_version 变更，方法缓存已填条目的 keys_version
   永久过期；而 fill 只填空槽——该类型方法查找**永久**落慢路径。
   修复：类型权威键版本已前移时驱逐过期条目（物化实例的瞬时不匹配
   不驱逐，保住 values 形态接收者命中）。

搭车：**整型常量累加器拒编阀移除**（穿刺前端 e31387ad5 整取件，无
文档化失败案例；仅"含方法调用或 localsplus>8"的大函数被拒，小函数
一直在编且语料全绿；其代价是 go 等计分型热函数整体不可编译）；
**simplify split-dict 投机的 WITH_HINT 站点证据门**（解释器已把站点
特化为 WITH_HINT 即物化形态为主，values 投机弃权——仅正向证据弃权，
INSTANCE_VALUE 与冷站点维持投机；auto=2 下站点多为冷态故本轮对 go
无感，暖编译档位受益）。

## 三、试错与回退：分支化 values 守卫

曾把 simplify split-dict 投机的 `dict values check` Guard 改为
CondBranch 回退缓存 helper（消除"天生物化"负载的 deopt 风暴→backoff
冻结）。实测 deltablue/raytrace 受益（-4%/-8%），但 go 解冻后全编译
形态整体劣于解释器行内 WITH_HINT（88→145ms：物化属性访问的 helper
调用税 × 每窗口 720 万次）。已回退，恢复"风暴→冻结"均衡；再落地
条件：DescrOrClassVar hint 化 + store 内联 stub 同轮完成，使编译态
物化访问成本贴近解释器行内水平。

## 四、量化（进程内稳态，同机同法）

| 基准 | 轮初 | 轮末 | 备注 |
|---|---|---|---|
| deltablue(100) | 1.88ms | **1.73ms（0.93x）** | 物化读写快路径 |
| raytrace(100²) | 160.9ms | **147.8ms（0.90x）** | 同上 |
| richards | 16.30ms | 16.07ms（1.37x） | 持平微升 |
| unpickle | 2.86ms | 2.85ms | ia 槽已接管（126k 次）但冻结均衡主导，待解冻轮 |
| go | 88.2ms | 103.3ms（-17%） | 拒编阀移除后更多函数入"编译→风暴→冻结"循环；根治项见五 |

19 基准 A/B（auto=2/w3/p3v5，19/19 全绿，存档 m9lag-ab-summary.json
+ 复测 m9lag-ab2-rerun.json）：**几何均值 0.838x → 0.858x**。
chaos 0.761→0.919、fannkuch 0.982→1.307、raytrace 0.809→0.884、
deltablue 0.883→0.900、pickle 0.626→0.721、scimark 0.739→0.825、
unpickle 0.750→0.779；richards/hexiom/generators 首跑离群
（1.267/0.551/0.677）复测回位（1.375/0.705/0.716，冻结均衡时序
方差）；**go 0.690→0.578 可复现回归**（拒编阀移除后更多函数进入
"编译→风暴→冻结"循环的代价；根治三件套见第六节，净收益判定以
几何均值 +2pp 为准）。

## 五、门禁

diffgate 923 全绿；generators 语料基线原样；基础 libtest 差分 0 新增；
refcount 矩阵五组零漂移；本轮定向冒烟（物化读/写/删除/重插/hint 失效
重定位/字典重排、实例属性方法位双形态+删除+类侧变化、共享键成长
自愈）与前两轮全部冒烟复跑通过；3.14 反向编译 + 四类冒烟通过。

## 六、遗留与后续杠杆

- **go 专项**（三件套后重评分支化）：DescrOrClassVar 读路径 hint 化
  （类默认值遮蔽形态 400 万次/窗口字符串哈希查字典——PMP memcmp
  之源）；store 内联 stub；"编译态劣于解释器"的普适性判定/回退策略；
- generators：生成器恢复链轻量化（帧/调用协议轴）；
- IsTruthy TBool 内联（richards PMP 榜首）；
- 计数采集器修正两坑已入 lag_stats 驱动（pickle 范围阀 + _pickle
  屏蔽），后续复采直接可用。
