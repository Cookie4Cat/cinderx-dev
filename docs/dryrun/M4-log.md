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
