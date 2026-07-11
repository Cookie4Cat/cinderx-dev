# M10 被调方行内压栈轮：调用位点入口缓存与直达入口

> 分支 dryrun/m10-call-entry-cache（基线 = v21/MR #78 合入态）。
> 目标：JIT→JIT 调用跳过被调方入口的 __code__ 身份校验与参数
> 计数/缺省/kwnames 仪式链，同时原样保留 tracing 分流与递归
> 余量预检两项语义。

## 一、设计

### 1. 被调方第四入口（direct_call_entry）

编译产物入口布局原为三入口（静态入口/绑参重入桩/vectorcall 入
口）。本轮在重入桩之前的安全区（分流出口同区，不触碰
JITRT_CALL_REENTRY_OFFSET=-12 硬不变量）新增第四入口：

```
direct_call_entry:
  [入口守卫序列]    tracing 激活 → 分流；递归将溢 → 分流
  b correct_args_entry   （复用绑参重入的建帧码，零重复）
```

两项开工前风险由该构造直接消解：

- **递归预检**：守卫序列与 vectorcall 入口行内形态共用同一发射
  函数（emit_entry_guard），语义一字不差；且直达入口无条件带守
  卫，不依赖 entryGuardInlined——直达路径绕过包装器形态，须自带。
- **tracing**：同守卫分流，x0-x3 处于原始 vectorcall 形态（x3=0
  见下），分流目标 _PyFunction_Vectorcall 语义与既有分流一致。

被跳过的两段的等价承担：

- **__code__ 身份校验** → 调用方缓存的 func_code 恒等守卫；
- **参数计数链** → 填充条件一次性保证（无 varargs/kwonly、位点
  实参数==co_argcount [32 位比较镜像 prologue 行内口径]、
  kwnames==NULL、非静态入口）。

入口地址经标签解析落 CompiledFunctionData.direct_call_entry，
miss helper 填充时经 lookupCompiledForCall→directCallEntry() 读取。

### 2. 调用位点入口缓存（CallSiteEntryCache）

per-site 单态缓存 {code, vectorcall, fast_target, helper_budget}
（Context SlabArena 常驻，偏移 0/8/16/24 由 static_assert 锁定，
地址烘焙进探测序列）。探测三守卫：

1. **精确 PyFunction 类型**——非函数直落旧慢臂槽（同时免除对非
   函数对象的越界字段读；内建密集位点的新增税 ≈ +4 指令）；
2. **func_code 恒等**——等价承担 __code__ 校验；
3. **vectorcall 恒等**——deopt/重编/试用转正/挂接改变入口后天然
   失效。

命中 `blr fast_target`。**双填充**：

- **直达填充**：被调方已晋升裸编译入口且直达条件全满足 →
  fast_target = 直达入口（本轮收益面）；
- **中性填充**：其余稳定函数 → fast_target = 填充时的
  func->vectorcall 值，命中语义与旧快臂逐调用槽装载一致——稳定
  而未编译/未转正/参数不整的被调方免于每调用落 helper。填充时
  恒等成立 ⇒ 派发即旧语义；code 亡后地址复用无害（直达入口地址
  由 bump 分配器保证终身不复用）。

**miss helper**（jit::JITRT_CallSiteEntryMiss，实现在 context.cpp
——需其文件内静态 lookupCompiledForCall）：函数形 miss 预算内经
x3 携 cache 进入（该类位点 kwnames 恒 NULL，改写发射 xor x3,x3，
miss 臂借位改载）；派发语义与旧两臂逐字一致。helper_budget=64 由
helper 递减，归零后探测序列行内短路 x13（vectorcall 已装载）＝
旧快臂形态，封顶多态位点的 helper 往返税。生命周期迁移（stock→
包装器→裸入口→挂接）由 vectorcall 恒等失效驱动预算内重填自然跟进。

### 3. 双位点覆盖

- **kCallSiteVectorCall**（VectorCall 位点）：如上。
- **kCallSiteCallMethod**（CallMethod 位点）：另带 callable 空/
  None（LOAD_METHOD 回落形）与 receiver 空槽（JITRT_Call 双 NULL
  约定）前置检查直落 g_JITRT_Call_slot（保留回落移位协议）；miss
  helper 共用——该路径 callable 已过精确函数检查，helper 的非函数
  兜底分支自方法位点不可达。

两操作码与 kVectorCall 操作数同构（[0] 改为 cache 地址 Imm），
postalloc 同款实参搬移改写但保留操作码，由专属翻译器发射探测。
TFunc 静态型位点与 KwArgs 位点维持原路径；excInjectEnabled 维持
helper 发射。旗标 jit-call-entry-cache / PYTHONJITCALLENTRYCACHE
（默认开）。

## 二、实现暗礁（实录）

- **rewriteCallInstrs 的"已改写形早退"判据是保留操作码调用形指令
  的必列名单**：判据 = numInputs==1 && output 已摘。漏列新操作码
  则第二遍改写按原始操作数布局 getInput(1) 越界（vector::at 抛
  std::out_of_range → 被编译外层捕获成 PYJIT_RESULT_UNKNOWN_ERROR，
  函数静默不编译）。gdb `catch throw` 直取抛点定罪。快路径家族
  （kLoad/StoreAttrCachedFastPath 等）当年同坑同修。
- 探测序列寄存器预算：调用形语义保证 x9-x15 此刻无活值（同
  IsTruthyFastPath 论证）；x16=reg_scratch_br 作目标寄存器，全
  路径汇于单一 blr，返回值/调试位置处理与 translateCall 同款。

## 三、验证

- **稳态行为**（gdb 断点 miss helper）：200k 调用恰 2 次 miss
  （每位点一次填充），填充为 DIRECT（fast_target = vectorcall
  入口 −0x34），直达入口反汇编逐字如设计（tstate 烘焙取址→
  use_tracing 字节读→cbnz 分流→recursion_remaining→b.le 分流→
  建帧→b correct_arg_count）；重入桩距 vectorcall 入口 −12 不变量
  原封。
- **语义冒烟** smoke_call_entry.py 八段：深递归 RecursionError
  （非段错误）、settrace 激活期 call 事件不缺失且关闭后恢复直达、
  __code__ 换体按新 code 语义、kwargs/缺省/kwonly/varargs 语义
  不变、多态位点（函数↔内建交替 300 轮超预算）、新鲜函数对象
  （lambda 翻新）、生成器创建。全套冒烟 15/15 过。
- **libtest 扩充带**（46 模块双臂）：1 分歧（test_dis，基线内），
  0 新增，2 项较基线转好。
- RCM 免跑说明：本轮不触碰引用计数语义（探测/填充/派发均无
  refcount 操作，miss helper 派发与旧臂逐字一致）。

## 四、定价

同二进制旗标 A/B（PYTHONJITCALLENTRYCACHE=1/0，稳态 best-of）：

| 面 | 仅 VectorCall 形 | +CallMethod 形 |
|---|---|---|
| richards per-iter | 中性（±1%） | **−1.0~1.6%** |
| deltablue best | 噪声级 | **−1.9%** |
| bpe_tokeniser best | −0.5% | **−2.6%** |
| deepcopy / go | 中性 | 中性 |

方法位点为主要收益面（richards/deltablue 热调用是方法派发）；
VectorCall 单独覆盖时中性——纯函数调用位点密度低，探测新增
（+2~6 指令）与被调方节省（约 12 指令）接近相抵；叠加新鲜函数
位点（bpe 推导式）与方法位点后转正。

全集 v22（PGO+LTO 口径，A 侧复用 v19-fresh 存档，存档
full113-ab-v22-summary.json）：86 项零失败，几何 1.038。

**漂移日裁决**：v22 与 v21 逐项比对中位 −1.8%，且纯 C 项同步回落
（pidigits −6.9%、gc_collect −5.7%——JIT 不可能经调用路径拖慢
_decimal/GC C 循环），判为 B 侧机器态漂移日（v21 为偏快日），
绝对几何不作轮次判据（红线：跨构建绝对值漂移 ±6%，单项判据以
受控进程内稳态为准）。最大回落族 networkx（−17~−21%）单独旗标
定罪：同二进制交错两轮 ON/OFF ±1% 无系统差，**与本轮无因果**
（该族本属宽噪族，v20/v21 两轮 +8pp/轮的上涨同样未经受控归因）。

漂移日之下关键靶项仍全部续涨（同一冻结 A 侧，涨=真信号强于
漂移）：**sqlalchemy_imperative 1.142→1.161 战役新高**、
sqlglot_v2_parse 1.158→1.202、sympy 1.013→1.043、
xdsl 1.079→1.117、django_template 1.081→1.118、
generators 1.075→1.112、sphinx 0.878→0.894、
coroutines 0.957→0.973。

## 五、遗留

- kwnames 位点（KwArgs 形）未覆盖——kwnames 恒等缓存是既档待办
  （kwnames tuple 每位点常量，可入缓存第四守卫）；
- TFunc 静态型位点维持单载荷直读（已近优）；
- 探测序列的 cache/PyFunction_Type 两个 mov-imm（各 3 指令）可改
  常量池 ldr literal（−4 指令/位点，i-cache 与派发延迟的权衡待
  950 实测后定）。
