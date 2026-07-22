# M10 解释器兼容轮:仅换解释器 Lib/test 差分收口

分支 dryrun/m10-compat。源起:用户问"只替换自定义解释器(JIT 关)时
Lib/test 差分哪些通过、能否全过"。全量扫荡(471 文件、40,220 用例、
auto=0 仅换解释器臂 vs 纯 stock 臂)后逐模块定性,三处机制性差异
落地修复,一处 PEP 523 插槽语义边界厘清。

## 一、全量扫荡:真实差分 5 模块

stock 臂与解释器臂的失败集之差(排除 10 个环境本底模块——
multiprocessing/signal/asyncio.test_runners 等 stock 同挂):

| 模块 | 例 | 首诊 | 终态 |
|---|---|---|---|
| test_dis | test_loop_quicken ×2 | PEP 523→拒 CALL 特化,位点终身 ADAPTIVE | 修(CALL 条件放行) |
| test_type_cache | load_attr/method/store_attr_user_type ×3 | 缺版本 0 拒特化守卫(陈旧缓存风险) | 修(版本 0 回迁) |
| test_type_cache | binary_subscript_user_type ×1 | GETITEM handler 运行期 DEOPT | 豁免(见五) |
| test_capi | Pep523API.inlined_binary_subscr ×1 | 同上,插槽独占 | 豁免(见五) |
| test__xxsubinterpreters + test_interpreters | test_in_thread 挂死 | 兜底导入把 threading 带入子解释器 | 修(惰性导入) |

**方法论纠错**:首诊曾归因 test_dis/type_cache 为"提前 quicken 时序
差"。证伪:`PYTHONJITEARLYQUICKEN=0` 回 stock 节拍后失败一个不少——
根因是机制性(PEP 523 拒特化 / 版本 0 缺守卫),非时序。旋钮
(PYTHONJITEARLYQUICKEN=0)仍成立且已存在,管的是内部状态快照逐字
对齐,不管这五模块。

## 二、子解释器兜底惰性导入

- 明文拒绝是设计行为(_cinderx-lib.cpp:999,非主解释器即 ImportError,
  保护进程级全局态;与 PEP 523 无关)。
- 挂死链:子解释器 site → sitecustomize → import cinderx → 兜底
  分支模块级 `from asyncio import ...` + `import asyncio` →
  concurrent.futures → logging → **threading 入子解释器** → 跨线程
  Py_EndInterpreter 等 threading._shutdown 锁,死锁。
- **决定性实证**:纯 stock + 单行 `import threading` 的 sitecustomize
  同样 3/3 挂——CPython 3.11 自身缺陷,cinderx 零参与(枪是上游的,
  扳机是兜底导入)。
- 修复:注解依赖 from __future__ annotations(字符串,无运行期);
  删模块级 asyncio 导入;import asyncio 移入 global 绑定处。
- **结果:两模块转全过**(All 2 tests OK)。

## 三、attr 特化版本 0 防护回迁

vendored 3.11.6 的 LoadAttr/StoreAttr/LoadMethod 缺上游后加的
`tp_version_tag==0` 拒绝守卫。零版本号(未赋出/耗尽)无法被特化
守卫校验,类型再变更版本号仍零→缓存命中即陈旧,**正确性风险**非
白盒噪声。三处函数入口回迁守卫。type_cache attr 三例转绿。

## 四、CALL 特化 PEP 523 条件放行(旋钮)

stock 见任何 PEP 523 钩子即永久拒 CALL 特化(specialize.c:1489)。
自家 Ci_EvalFrame 是唯一常驻租户,此拒绝对自身多余——致解释层
Python→Python 调用位点终身 CALL_ADAPTIVE(**hexiom 直方图
CALL_ADAPTIVE×100 之根**同案告破)。放行判据
(eval_frame_blocks_specialization):开关
`PYTHONJITPEP523SPECIALIZATION`(默认关)开 **且** interp->eval_frame
恰为 Ci_EvalFrame 才放行;第三方钩子维持 stock 拒绝。默认关=交付态
逐字等价(实证:旗标关时 test_dis 仍 FAILURE);旗标开为仅换解释器
兼容口径准备。test_dis 二例转绿。

## 五、PEP 523 插槽语义边界(不可调和,豁免 2 例)

关键取证:**唯 BINARY_SUBSCR_GETITEM 的 handler(ceval.c:2251)带
运行期 `DEOPT_IF(tstate->interp->eval_frame)`**;CALL_PY handler 无。
故:

- CALL 放行有效(handler 不 DEOPT,test_dis 转绿);
- **GETITEM 放行无效**:specialize 写 GETITEM,handler 执行期见
  eval_frame 立即 DEOPT 回 ADAPTIVE。type_cache.binary_subscript
  (should_specialize=True 要 ADAPTIVE 消失)因此在 PEP 523 环境
  无论放行与否都红。
- 真正启用需**同步条件化 handler 的 DEOPT_IF**(改动执行语义:
  __getitem__ 内联进本循环)——属独立性能轮(需 RCM+A/B 定价),
  不在兼容轮范围。故 GETITEM 放行**已回退至 stock**。
- test_capi.inlined_binary_subscr(装第三方记录钩子,断言
  __getitem__ 帧可见)与 binary_subscript 同因(GETITEM 内联 vs
  PEP 523 帧可见性),结构性冲突。

**两例豁免同根**:GETITEM 内联特化与 PEP 523 帧可见性不可同时满足;
可由后续性能轮(handler 条件放行+定价)一并关闭,兼容轮不修。

## 六、验证与最终矩阵

交付态(旗标关):diffgate 940 例 0 新增(+4 修复);RCM 8 组同臂
全等;冒烟四件过;richards 0.00804 / hexiom 0.00918 B4 无回归
(版本 0 守卫仅拒退化态,结构 perf 中性)。

兼容口径(旗标开)Lib/test 全量豁免清单:
- **type_cache.binary_subscript + capi.inlined_binary_subscr**
  (2 例,GETITEM 族,可由性能轮关闭);
- 口径说明:①环境本底 10 模块 stock 同挂不记账;②JIT 开启口径
  另有已知小面(test_scope testLeaks 等)。

40,220 用例、豁免 2 例——PEP 523 形态解释器产品的实用最优面。

## 七、遗留/后续

**GETITEM 解释层内联特化(性能轮)**:条件化 CALL/GETITEM handler
的 DEOPT_IF(仅自家求值器放行 + 第三方钩子装入时正确 DEOPT),启用
解释层调用/下标内联特化。预期收益面=hexiom 驻留 12% + 温函数带
CALL_ADAPTIVE 本底;需 handler 执行语义正确性(内联帧 refcount、
换钩子时 DEOPT 时序)RCM + 定向 A/B。可一并关闭本轮豁免的 2 例。
