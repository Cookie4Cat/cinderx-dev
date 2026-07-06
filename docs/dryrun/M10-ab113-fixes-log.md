# M10 第一轮：1.13 全量摸底五失败项销案（三根因家族）

日期：2026-07-06　分支：`dryrun/m10-ab113-fixes`　基线：交接终态
（19 项诚实口径 1.059；pyperformance 1.13 全量 86 项摸底几何均值
0.923，B 侧独有失败 5 项——摸底数据见 full113-ab-summary.json）

## 〇、摸底与任务

pyperformance 1.13.0 在 3.11 的适用集 86 项（= 3.14 的 95 项减 9 个
`>=3.12` 门控项；hg_startup 上游注释禁用）全量 A/B：81 项出数、5 项
B 侧独有失败（A 侧全绿）——networkx 三变体（AttributeError）、
regex_v8 与 sqlalchemy_declarative（SIGSEGV）。本轮任务：五项全部
销案。跑批器 run_ab_full113.py（MANIFEST 驱动 + 版本门过滤）随轮
入库。

## 一、家族一：IC 提示读撞已删除条目（sqlalchemy_declarative SIGSEGV）

验尸：核心转储 `DescrOrClassVarMutator::getAttr →
ci_peek_instance_attr_hinted_311 → getDictKeysIndex →
PyUnicode_Compare`，si_addr=0x8（NULL 解引）；现场 obj 为 sqlalchemy
`Mapper` 实例（物化字典 59 条含已删除条目），name="logger"。

根因：`Common/dict.h::getDictKeysIndex` 线性扫描未跳过
`me_key == NULL` 条目。3.11 unicode 键字典的 delitem 语义把已删除
条目留在 dk_nentries 界内（me_key/me_value 置 NULL）。上游调用方
只传共享键（永不删除）故从未暴露；3.11 移植把该函数扩用到物化
实例字典（combined 键）——SQLAlchemy 恒常做属性过期删除，确定性
命中。

修复：扫描跳过 `me_key == NULL`（一行 + 注释）。最小复现（15 行，
smoke_m10_fixes.py::check_ic_deleted_entry）：撑爆共享键容量迫使
自然物化 + delattr 留删除条目 + 多态站点（单态会被编译期烘焙绕过
运行时 IC——首次复现失败的教训）。

## 二、家族二：JITRT_Call 的两种配对约定并存（regex_v8 SIGSEGV）

验尸：`JITRT_Call(callable=0x0, kwnames=("count",))`。入口哨兵定位
到 bm_regex_v8 的 block11（22KB 字节码巨函数）中
`re.sub(..., count=0)` 调用。

根因是一个"约定碰撞"：3.11 上到达 JITRT_Call 的调用配对有两种
形态，原实现只认其一——

1. **原始字节码形态**（NULL 在 callable 槽）：带关键字的调用 3.11
   编译器不用 LOAD_METHOD，改用 `LOAD_GLOBAL(NULL 标志)/PUSH_NULL +
   LOAD_ATTR`，第一槽为字面 NULL。simplifyCallMethod 通常在编译期
   消解该 NULL，但 simplify 有迭代/新块预算（new_block_limit=1000），
   巨函数预算中途耗尽后剩余 CallMethod 原样漏到运行时——**正确性
   不得依赖带预算的优化 pass**。
2. **归一化形态**（NULL 在第二槽）：`LoadMethodResult` 构造函数在
   3.11+ 把解释器的 None 约定统一转为 `{callable, self_or_null}`
   （3.12 顺序），全部方法缓存 helper 由此产出。

修复：JITRT_Call 按 NULL 位置双分支判定（合法实参不可能为 NULL，
判定无歧义）：callable 槽 NULL → 真 callable 在 args[0]，移位；
args[0] NULL → 丢弃该槽。排障实录（红线级）：首版修复只认形态 1，
把形态 2 的归一化配对误判——classmethod 经实例方法缓存慢路径返回
{bound, NULL}，NULL 被当实参传入 method_vectorcall 原地插桩后以
[self, NULL, ...] 进入被调函数入口（enum 动态建类 9 行即崩）。
**同一助手面对多来源输入时，须以"来源清单全枚举"验证修复，而非
只验证触发用例。**

## 三、家族三：__code__ 替换后编译入口不失效（networkx 语义分歧）

验尸：AttributeError 栈指向 networkx argmap 惰性编译装饰器；其设计
为"首调后 `func.__code__ = real_func.__code__` 自替换"。5 行最小
复现：JIT 编译后换 __code__，调用仍执行旧函数体（语义要求执行新体）。

根因：3.11 无 function watcher（3.12 引入），编译入口装在
`func->vectorcall` 后对 __code__ 替换无感知，永远执行编译时旧体。
连锁后果即 argmap 的 __wrapper 链错乱（位置实参生成器串位进 kw-only
形参）呈现为 AttributeError。

修复：vectorcall 入口无条件发射 **code 身份拉式校验**（4 指令）：
`ldr func->func_code; cmp 烘焙 code 指针; b.ne 分流`，不符整调用
转交 stock 解释器入口（按新 code 正确执行，后续 auto-JIT 对新 code
自然重新计数编译）。分流出口与入口守卫共用（守卫可配置关闭，本
校验无条件）。闭包多函数共享同一 code 的形态不受影响（比对的是
code 恒等）。绑参重入路径在校验之后进入，外层已检。遗留注记：
Static Python 静态入口未走本校验（本目标不用 Static Python，随
专项另议）；x86-64 3.11 入口如启用需同等处理。

## 四、门禁与量化

门禁：冒烟五件全过（新增 smoke_m10_fixes 固化四个最小复现，双模
验证）；diffgate 923 全绿；refcount 矩阵六组零漂移；libtest 相对
基线无新增分歧（仅既有 test_builtin 微漂移 / test_scope 跟踪项）；
3.14 反向编译 + 两项 smoke 通过。终态构建 = PGO 三相配方重跑
（161 gcda，与上轮一致）。

终态 A/B（pyperf p3/w3/v5，存档 m10-verify-ab-summary.json）：

- **五个失败项全部销案且出数**：networkx 1.003 /
  networkx_connected_components 1.027 / networkx_k_core 0.999 /
  regex_v8 **1.056（转绿即反超）** / sqlalchemy_declarative 0.890；
- **抽查无回退**：richards 2.281、richards_super 2.269、deltablue
  1.273、go 0.661、sqlalchemy_imperative 0.681——均在历史存档的
  跨构建 ±6% 漂移带内，入口 code 身份校验（每调用 4 指令）对热
  路径不可见。

至此 pyperformance 1.13 全量 86 项在 3.11 上无已知崩溃面与语义
分歧面（B 侧独有失败清零）；落后项全部为已知"编译价值分化"形态
（策略层专项范围）。

## 五、方法论沉淀

- 全量摸底（86 项 vs 惯用 19 项）单轮即捕获 3 个根因家族——测量
  面扩张本身就是最高效的缺陷发现手段；正式项目 Daily 门禁应含
  全量集；
- 入口哨兵（NULL 即 fprintf 现场 + abort）比核心转储寄存器考古
  快一个量级，巨函数/优化产物场景优先用；
- 编译期归一化（simplify）与运行时防御必须成对存在：凡"优化 pass
  保证不变量"处，运行时入口须能容忍不变量缺失（预算/超时/跳过），
  否则优化预算就是正确性边界；
- 无 watcher 版本上，一切"编译产物 ↔ 可变运行时对象"的绑定都要有
  拉式校验；本轮补上 func→code 一环（类型/字典/全局已有 D5 拉式
  体系，函数对象此前是缺口）。
