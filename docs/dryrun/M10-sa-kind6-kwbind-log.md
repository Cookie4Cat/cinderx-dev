# M10 sqla 残项②③:kind-6 成员写行内与 kwnames 绑定缓存

分支 dryrun/m10-sa-kind6(基于 !80 合入态),两提交。

## 一、kind-6 成员写行内(a9c5d0717)

__slots__ 成员写(PyMember_SetOne T_OBJECT_EX 形)此前一律经 C
helper(sqlalchemy_imperative 直方图 10.5 万次/10 趟)。sa 桩 kind
派发新增 kind-6 支线:条目侧装载 memberdef 进共享写块。准入:
T_OBJECT_EX 且 flags==0;删除形与旧值末引用(dealloc 任意重入)
回落 helper。引用序:预检旧值计数→减旧→增新→店(old==value
别名下 N≥2 恒成立,先减后增净零)。

排障实录:条目字段装载须为 entriesOffset()+mutatorFieldOffset()+
entry_offset 三段和——漏加数组基偏移会把版本字段值当指针解引用
SIGSEGV;单条目冒烟恰不错位,多条目真负载才暴露(gdb 崩溃点
寄存器定罪)。

收据:sa_hit_kind_6 10.5 万→0,sa_invoke 40.3 万→29.8 万(−26%);
新增 smoke_sa_kind6 五段(同对象重赋值计数净零/换值交接
getrefcount 对照/删除重写/dealloc 路径);RCM 八组改动前后 jit 臂
漂移集全等(矩阵在 AUTO 下的 roi 冻结一次性迁移漂移为既有老账,
与本改动无关,已另立追查项)。

## 二、kwnames 绑定缓存(d96fec343)

JITRT_BindKeywordArgs 每次调用对每 kwname 做 posonly..total_args
指针扫描(sqla 剖面 1.4%;调用位点 kwnames 为 LOAD_CONST 元组,
映射恒定)。CodeExtra 增设单条目缓存 {nkw, names[10], slots[10]}:

- 无所有权设计:缓存名指针只比较不解引用,命中判据=逐索引与活
  元组条目身份相等——ABA(元组亡后地址复用)下同名驻留串同指针,
  比对通过即映射构造性正确;规避强引用方案需改 co_extra freefunc
  (PyMem_Free 直释)的契约面;
- 仅缓存快速指针比对全命中且无 varargs/varkw、nkw≤10 的形态;
  富比较回落形与 kwdict 形不缓存;命中路径保留重复赋值检查与
  缺省值活读(func_defaults 可变),语义与慢路径逐字一致。

收据:kw 密集微基准同味双构建 A/B 0.0252→0.0218 s/20 万调用
(−13%)。

## 三、门禁与全集

冒烟 16/16;libtest 46 模块 0 新增(2 转好);RCM 八组同臂全等;
PGO 链验收 OK。全集 v24(存档 full113-ab-v24-summary.json):
86 项零失败,几何 1.046 持平 v23,≥1.0 项 58→59,逐项全部带内
——本轮为微观面收益(计数器与微基准为收据),全集层面零回归
即合格,叠加总账待 950 复测。

## 四、遗留

lm per-entry hint(9.7 万残余);RCM 矩阵 roi 冻结迁移漂移老账
(独立追查项);telco/websockets DSO 定罪与 pathlib/concurrent_imap
补测(落后带验尸轮继续)。
