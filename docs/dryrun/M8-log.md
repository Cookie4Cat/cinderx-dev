# M8 预演日志：Generator 基础支持（D6：仅同步生成器可编译）

日期：2026-07-04　分支：`dryrun/m8-gen`　基线：`dryrun-311-base@10c3b2036`（M7 合入后）

范围（设计书 M8 行）：sync gen send/throw/close/yield-from、挂起点 deopt
安全、协程/async gen 拒编测试；gen 不走 LWF。ASAN 专项以 PYTHONMALLOC
调试分配器 + 差分语料代偿（预演容器未插桩）。

## 一、现状与 D6 拒编阀

修改前三态：sync gen / 协程均 `UNKNOWN_ERROR`（M1 期 buildHIR 对
`kCoFlagsAnyGenerator` 一刀切 JIT_THROW），async gen 已有干净拒编。

落地 D6：eligibility 层 `forbidden_flags`（pyjit.cpp）在 `< 0x030C`
扩为 `CO_ASYNC_GENERATOR | CO_COROUTINE | CO_ITERABLE_COROUTINE`（干净
CANNOT_SPECIALIZE 回退解释器）；buildHIR 阀改为仅拦异步类作后备。
修改后：sync gen 编译成功，协程/async gen 均 CANNOT_SPECIALIZE（出口③
拒编路径测试通过）。

**gen 机器本体（GenDataFooter/JitGen 类型/resume 管线）在 M3/M4 取料时
已随版本门就位**——开闸后协议冒烟（next/send/返回值/throw/close/
yield-from/gi_frame 自省）开箱即绿；M8 的实际工作量在下述隐蔽的
引用会计与生命周期缺陷。

## 二、核心战果：3.11 生成器引用会计双缺陷（互相掩护）

`corpus_generators` 首跑 jit 模式第 2 用例即段错误，定位链两层：

**缺陷一：`gi_code` 从未初始化。** 3.11 的 PyGenObject 头部仍有
`gi_code` 强引用字段（3.12 起移除、code 移入嵌入帧），JIT gen 的
初始化按 3.12+ 布局写字段，从不写 `gi_code` → 分配器残留垃圾（实测
0x17）→ stock `gen_traverse` 访问它，**挂起期任何 GC 即崩**。此前
六个里程碑未暴露纯属语料中无"gen 存活期间触发 GC"的用例。
修复：出生时 `gi_code = Py_NewRef(co)`（jit_rt.cpp）。

**缺陷二：`f_code` 借引用被错误递减。** `gen_dealloc_with_custom_free`
的 `Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE)` 按 3.12+ 语义释放帧对
code 的强引用；3.11 帧的 `f_code` 是**借引用**（强引用由 `f_func`
持有、`_PyFrame_Clear` 负责），该行为每次 gen 析构对 code 对象多减
一次引用。

**两缺陷恰好抵消**（漏配 +1 与错减 −1），单独修复缺陷一后失衡立即
爆发：code 对象过早释放，表现为编译期 `co_names` 垃圾（checkTranslate
strlen(NULL) SEGV）、恢复点错位执行（"line -1"、属性名成 tuple、
finally 静默跳过）、关停期 fmt 空指针（code.cpp:212）——三种远端症状
同根。修复：该行加版本门，3.11 改为与 stock `gen_dealloc` 对齐的
`Py_CLEAR(gi_code)`。**教训（正式开发红线）：跨版本引用会计变更必须
成对审计"谁持强引用"，且两处错误相互抵消时单点修复反而引爆——refleak
工具对 gen 语料的专项覆盖应作为 M8 正式开发的先决门禁。**

## 三、corpus_generators 差分套件（22 用例）

协议矩阵：send（含未启动 send）、throw（内捕/穿透）、close（含
finally、析构触发、for-break 触发）、yield-from（返回值/throw 穿透/
递归委托）、PEP 479、多实例交错、cellvar、大局部变量跨 yield、热恢复
循环、genexpr 跨界消费、gi_frame 自省、挂起点 checkpoint deopt ×4。

双缺陷修复后，带崩溃恢复的全量门禁（940 用例 = 918 + generators 22）
给出终态：**六项失败 = 3 个独立问题 × 两模式**，其余全部与解释器逐项
一致（close/dealloc/finally 族、yield-from 族、checkpoint deopt 族、
genexpr、cellvar、PEP 479 之外的异常协议全绿）：

| 类别 | 用例 | 定性 |
|---|---|---|
| PEP 479 未落地 + 运行态残留 | pep479（SEGV） | 编译 gen 内 raise StopIteration 未洗白为 RuntimeError 直接外漏，且异常退出未复位运行态（后续操作报 generator already executing）；完成路径 3.11 分支缺口，移交正式 M8（独立复现脚本在库） |
| 序列依赖 SIGILL | gi_frame_lineno（套件内崩溃、单独运行通过） | 与 pep479 案的状态残留疑似同源，需 ASAN/内存级定位，移交 |
| 挂起帧 locals 空 | gi_frame_locals（`[]`） | M3/M6 已立案 locals 缺口的生成器形态，属既定移交项 |

另修复第三处 3.11 布局缺口：**活体 JIT gen 缺 `gi_code` 属性**（stock
3.11 经 member 表暴露、JitGen 类型按 3.12+ 结构生成未含）。getset 注入
方案触发类型初始化的逐下标同名拷贝协议断言（全量 SIGABRT，门禁当场
抓获），改按 stock 方式经 `tp_members` 补齐后，frames 模块既有的
`gen_frame_lineno_while_suspended` 用例（此前因 gen 拒编从未走过 JIT
路径）恢复通过。

## 四、验证

- diffgate 全量（918 + generators 22）：既有 918 用例 0 失败（全绿维持）；
  generators 模块失败按上表分类，快照入 M8 基线；
- refcount 矩阵（calls/frames × interp/jit）：四组 0 漂移 —— 引用会计修复
  未引入既有语料漂移；
- PYTHONMALLOC=debug 下 close/析构/GC 复现脚本全程干净；
- 3.14 反向回归：ninja 全量 0 错；gen 协议冒烟（send/返回值/GC 共存/close+finally）通过。

## 五、移交（正式 M8 范围）

PEP 479 与完成路径运行态复位（含序列依赖 SIGILL 同源排查，建议 ASAN
首个专项）、挂起帧 locals 重建（并入 M6 locals 主体）、refleak gen
专项、异常注入 fuzz 对 gen resume 路径的检查点扩展（本轮因崩溃案
未扫）。

预演工时：约 3 小时。
