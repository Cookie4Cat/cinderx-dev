# M4 预演记录：3.11 字节码前端适配（第一轮）

> 起始：2026-07-04 ｜ 分支 dryrun/m4-frontend ｜ 基线 dryrun-311-base（M3 已合入 @05d0f459f）
> 本轮 = M4 的首个时间盒：销 M3 立的案 + 语料上机拿基线 + 按聚类修系统性 bug。

## 首案销案：calls heisenbug = ABI 参数错位

- **根因**：LIR 的 kVectorCall 在 3.11 上把 `_PyObject_Vectorcall`（4 参）替换为
  `JITRT_VectorcallPythonFunction`，但后者签名带了一个**没人用的前导
  `PyThreadState*`（5 参）**。调用点按 4 参装载 x0–x3 → helper 眼中所有
  参数左移一位，把栈上 args 缓冲当 PyFunctionObject，从"它的 vectorcall
  偏移"读出栈垃圾当函数指针跳转。gdb 在 helper 入口抓到形参整体错位
  （args=0x8000000000000001 即 nargsf 的值），铁证。
- **修复**：签名对齐为 `_PyObject_Vectorcall` 同款 4 参（头文件注释写明
  "drop-in 替身必须保持精确签名"约束）。
- **教训**：**替身 helper 的签名必须与被替换符号逐参一致**——这类错位
  编译器完全不报（LIR 调用是免检的 uint64 地址）；heisenbug 表象
  （PYTHONJITDEBUG 掩盖、拒编后"状态污染"假象、LIR Imm{0} 断言）全部
  同根。正式开发建议：LIR 层替身注册处加 static_assert 比对函数指针类型。

## 语料基线与两族系统性修复（118 → 86）

diffgate 918 case × 3 模式首跑：**118 失败**（对照穿刺 75/347——语料翻倍
后失败率反而低得多）。聚类修复：

1. **qualname 前缀族（26 条，M0 已知④族）**：`JITRT_CallFunctionEx` 报错
   用老式 `PyEval_GetFuncName`（裸名），上游 3.9+ 用 `_PyObject_FunctionStr`
   （限定名）+ 先清异常再格式化。按上游语义对齐——注意**此偏差在 3.14 上
   同样存在**，只是 kunpeng 语料没覆盖过；本修复双版本同惠。
2. **NameError 乱码名族（6 条，M0 已知①族）**：fallback 手写的
   `Cix_format_exc_check_arg` 把 `PyObject*` 直接喂给 `%.200s`（按 char*
   读对象内存 → 打出 refcnt 字节，`'\x06'`/`'\x02ʚ;'` 之谜破解）。按上游
   逐字对齐（AsUTF8 + NameError name 附加——did-you-mean 建议机制依赖）。
   **fallback 手写镜像第四次被抓**。

复跑：**86 失败，两族全清零回归**。

## 一次错修的教训（评审素材）

曾按 M1 的 loadAttrIndex 类比给 `loadGlobalIndex` 加 `<030C` 裸索引守卫 →
语料 118→332 恶化，立即回退。**事实**：LOAD_GLOBAL 的低位 NULL 编码是
**3.11 就有的**（CALL 协议重排的一部分）；3.12 才引入的是 LOAD_ATTR 的
方法位。已在 code.cpp 加注释钉死，防止后人再"修"。**语料门禁在 15 分钟内
抓住了错修——这就是它存在的意义**（比 review 更快更硬）。

## 剩余 86 失败的家族分布（正式 M4 清单）

- **crash 59**：已识别子族——(a) 大整数运算（op_*__bigpos 系，SIGABRT=
  JIT_CHECK 拦截，单跑不复现、仅在全量 harness 累积状态下发作 →
  特化/quickening 状态相关，需 diffgate 的崩溃隔离归因跑）；(b) unbound
  局部变量错误路径（fstring_unbound/augassign_before_bind SEGV—— CheckVar
  守卫的 3.11 错误路径）；(c) deopt 模式特有若干。
- **other 27**：待聚类（多为行为差异）。
- 前端拒编阀触发清单（raise-only、int 累加器、try-loop-handler、
  generators）= 翻译支持缺口，正式 M4 逐项转实现。

## 验证

- 3.14 反向回归 0 错（qualname 修复属双版本行为修正，向 stock 对齐）；
- 语料基线 round3 报告存档 docs/dryrun/m4-diffgate-baseline-round3.json。

## 工时

heisenbug 定位+修复 ~50 分钟（gdb 三轮：崩点反汇编→调用方→helper 入口
形参错位）；两族修复+一次错修回退 ~1 小时；三轮全量语料 ~40 分钟。
**本轮合计 ≈ 2.5 小时**。

## 遗留（正式 M4 清单）

1. crash 59 的三个子族逐一销案（bigpos 需 harness 状态复现管线）；
2. other 27 聚类；
3. 拒编阀清单转翻译支持；
4. opcode 三态覆盖矩阵未产出（M4 出口②）；unbound 矩阵未全绿（出口③）；
5. LIR 替身签名 static_assert 防线；
6. auto-JIT 引导接入（届时 `Ci_MaybeScheduleAutoJIT` 与 M2 直通版
   interpreter.c 合流）。

## 第二轮（同日）：118 → 74 → 21，三个系统性家族歼灭

1. **LOAD_FAST 未绑定检查缺失**（unbound 错误路径 SEGV 族）：3.11 没有
   LOAD_FAST_CHECK/LOAD_FAST 之分（3.12 的确定性赋值分析产物），移植版
   只对 LOAD_FAST_CHECK 发 CheckVar → 3.11 普通 LOAD_FAST 读未绑定局部
   直接把 NULL 喂给下游（PyNumber_InPlaceAdd(NULL) 段错误）。修复：
   3.11 上每个 LOAD_FAST 发 CheckVar（与解释器一致）。清 12 案。
2. **csel 模板 vs LIR 立即数传播**（"59 crash 雪崩"的真源头）：操作数
   拷贝传播把常量 Move 折进 Select 输入，aarch64 csel 模板要求全寄存器
   → JIT_CHECK abort → **单进程 harness 后续全部 case 雪崩记崩**（59 个
   crash 实为 1 个 bug 的下游）。触发条件教科书级：shape 函数先执行
   （vendored specialize 完成 quickening）后再编译同族 case 才可达。
   修复：模板内 Imm 物化到 x13/x14（DISALLOWED 集合内的专用 scratch）。
3. **单例不朽性假设烧穿 3.11**（关停期 FatalRefcountError 族）：
   PrimitiveBoxBool 把 Py_True/Py_False 当"新引用"返回但不 incref——
   3.12+ 靠 PEP 683 无害，3.11 每调用净 -1，True 被 dealloc
   （bool_dealloc Fatal，逐 case gc.collect 全绿定位到关停期，skip 二分
   → op_ge__int3__int0，活体 refcount 探针 dTrue=-1000 实锤）。修复：
   按端口自己的 CIX_PSEUDO_IMMORTAL 设计，在 Ci_InitOpcodes 把五个单例
   （True/False/None/NotImplemented/Ellipsis）伪不朽化——一处修复覆盖
   全部"借用不 incref"路径。小整数不纳入（其路径已带显式 incref，
   全面不朽化会致盲 refleak 工具）。**refleak 工具需特判伪不朽对象**
   （M2 遗留清单追加）。

## 剩余 21（正式 M4/M7 清单）

- crash 3 案 ×2 模式：annotation_only_then_read / read_before_assign /
  walrus_dead_branch——同为"imm vs aarch64 模板契约"家族第三例
  （translateMove 的 VecD 路径，编译期 SIGABRT，复现 10 行）。**正式修法
  建议升维**：与其逐模板加固，在 LIR 拷贝传播/后分配层给"模板不可接受
  imm 的操作数位"建统一约束表——三例同族已证明逐处补是打地鼠。
- other 15：attr_class 系 8 条 = **M0 早已分类的"IC 对类变异不失效"**
  （watcher 空桩的必然，M7 版本号守卫的既定范围，非 M4 缺陷）；
  match 语句系 3 条 + 杂项 4 条待 M4 正式聚类。

## 第二轮工时

~3 小时（LOAD_FAST 族 40 分钟；csel 族定位 1 小时——含 quickening 触发
条件推理；不朽性族 1 小时——gc 定位 + skip 二分 + 活体探针；收尾 20 分钟）。

## 第三轮（07-04）：语料 21 → 9，M4 范围内失败清零

1. **"无可达正常返回"函数的编译处理**（剩余 3 项编译期中止的根因）：
   全部路径均被必然抛出的守卫（如静态未绑定局部变量的 CheckVar，输出
   Bottom 类型）支配的函数，SSA 简化后 HIR 中不再有 Return 终结符；
   3.11 材料化帧的出口块以正常返回的 phi 汇聚返回值，零输入 phi 在
   后续改写中产生空操作数，代码生成无法处理。处置：在 LIR 生成入口
   增加检查（与前端既有模式阀并列），此类函数拒绝编译并回退解释器，
   行为与解释器一致（三个用例的 UnboundLocalError 逐字符一致）。
   检查置于 LIR 层而非 buildHIR：死 Return 由 SSA 简化阶段剪除，
   buildHIR 时尚存在。3.14 不受影响（轻量帧尾声不经出口 phi）。
2. **MATCH_* 操作码在 3.11 上改为拒编**：运行期助手在 borrow fallback
   中为占位实现（第五处占位实现，此次表现为运行期 RuntimeError 与
   解释器行为可见不一致），拒编回退后行为等价。match 语句的翻译支持
   列入正式 M4 工作项。
3. **opcode 三态矩阵产出**（M4 出口条件②）：89 个操作码"已编译且
   deopt 模式通过"，缺口恰为 MATCH_CLASS/MATCH_KEYS/MATCH_MAPPING
   三项，与失败清单互相印证。工具与矩阵文件已入库
   （docs/dryrun/opcode_tristate_matrix.py、opcode-tristate-matrix.json）。
4. **unbound 矩阵全部通过**（M4 出口条件③）：r3final 报告中
   corpus_unbound 家族零失败。

### 终态（r3final 基线，docs/dryrun/m4-diffgate-baseline-r3final.json）

9 项失败 = attr 类变异后缓存未失效 8 项 + descriptor 类 1 项（仅
jit_deopt 模式），全部属于 M7 版本号守卫的既定工作范围（watcher 在
3.11 为占位实现的必然结果，M0 阶段即有分类）。**M4 范围内失败为零**。

### 第三轮工时

~2 小时（出口 phi 问题的定位与两次定位修正 ~1.2 小时——教训：修复
位置需以"该信息何时可得"为准，Return 剪除时机决定检查必须在 SSA 之后；
MATCH 拒编与矩阵工具 ~40 分钟）。
