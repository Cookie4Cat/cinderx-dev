# M3 预演记录：Frame 基础模型（+ M4 前端预取）

> 起始：2026-07-04 ｜ 分支 dryrun/m3-frame ｜ 基线 dryrun-311-base（M2 已合入 @36c5aa9f4）
> M3 出口需要真实 JIT 帧在栈上 → 从 dryrun/m1-full-port 预取字节码前端
> （builder/preload/simplify/lir/jit_rt/frame/gen_asm/pyjit 功能层，PLAN 原本
> 就把 M3+M4 捆在同一晚）。拒编安全阀随 full-port pyjit 替换而移除。

## 头号战果：M1 遗留 SEGV 击毙（根因与 M1 的猜测不同）

- **现象**（M1 起）：force_compile 后首次调用即段错误。
- **根因**：`Config::frame_mode` 默认值只看编译期 `ENABLE_LIGHTWEIGHT_FRAMES`
  （kunpeng 已把 3.14 的 LWF 做成默认），**无版本意识**——3.11 运行时默认
  进 LWF 内联帧建表；而 LWF 的编译期字段表在 3.11 形状下**跳过
  frame_obj/stacktop/is_entry**（3.14 靠懒 reify 补，3.11 没人补），解链时
  读到垃圾 `frame_obj=0x5` → SEGV。gdb 帧解剖（f_func→prev_instr 有效、
  frame_obj/stacktop/is_entry 垃圾）与字段表逐项吻合。
- **修复**：config.h 默认值加 5 行版本条件（<0x030C 默认 kNormal）——
  正是 D4 的字面落地（LWF 骨架保留、默认关）。修复后 JIT 编译代码在
  3.11 上首次正确执行（fib 递归/循环/属性/跨帧全对）。
- **教训归档**：M1-log 猜的"co_framesize 替代公式"是无辜的；真凶属
  "kunpeng 新特性无 3.11 意识"家族（behavior_classifier 同族）。
  cmake_options.py 里验证分支留下的"3.11 开 LWF"配置注释自己就写着
  "正式开发按 D4 改"——**预演口径与设计决策冲突的配置要在移植时立即
  对账，不能等崩**。

## 第二战果：逃逸帧 take_ownership（D9 级正确性 bug）

M1 fallback 手写的 `_PyFrame_ClearExceptCode`（3.13 名字、3.11 没有此函数）
缺上游 `_PyFrame_Clear` 的 take_ownership 分支：逃逸的 PyFrameObject 指着
已死的解释器帧（悬垂→潜在 UAF）且永不进 GC 跟踪。修复：委托 vendored
逐字 `_PyFrame_Clear`（含 take_ownership 数据搬移、f_back 链接、GC track），
用一次 pre-incref 平衡 code 引用（两条路径的引用账均配平，注释内证明）。
**fallback 手写镜像第三次被抓**（前两次：MakeAndSetFrameObject 桩、
AsyncGenValueWrapperNew 桩）——"手写镜像 vs 逐字 vendor"的可靠性差距
已有三个实证，正式开发的 fallback 应全面转逐字抽取。

## M3 出口验收：reify 冒烟五件套 = 7/8 绿

| 项 | 结果 |
|---|---|
| getframe（名称/行号） | ✅（行号精确到 getframe 调用行） |
| 跨界 f_back 链（interp→jit→interp） | ✅ |
| traceback 行列与消息 | ✅ |
| **locals 快照** | ❌ `{}`——见下 |
| gc 可见性 + collect 存活 | ✅✅（take_ownership 修复后） |

**locals 快照缺口（M3 正式工作量，根因明确）**：JIT 寄存器分配的 locals
从不写入 `frame->localsplus`（材料化帧初始化为零后不再更新），frame 的
f_locals 读到空。需要在 getframe/逃逸/解链时按 DeoptMetadata 重建
localsplus——端口的 deopt 帧重建（reifyLocalsplus，来自异常路径修复提交）
就是同一套机制，复用它做"任意点 locals 物化"即 M3 正式主体。
f_lineno 已正确说明 instr_ptr 物化是通的，缺的只是值重建。

## 前端拒编安全阀实测（M4 情报）

验证分支给 3.11 前端装了模式级拒编（buildHIR 前奏 JIT_THROW）：
generators（D6 预期）、try-loop-returning-handler、只 raise 退出的函数、
int 常量累加器模式。实测两条被触发（raise-only、int 累加器）——阀有效，
但**清单即 M4 的翻译缺口清单**。

## 新发现：MAKE_FUNCTION/闭包调用 heisenbug（M4 头号靶子）

`def calls(n): def inner(x)... ; inner(t)` 模式：编译成功、**执行段错误在
生成的机器码内**；带 PYTHONJITDEBUG 则完全正常（内存填充差异掩盖）。
同根异象：拒编后再编它 → LIR `operand [type=Imm,val=0x0] as physical
register` 断言（release 下 JIT_CHECK 仍激活抓到一次）。特征指向 codegen
路径存在未初始化 LIR 操作数。**确定性 10 行复现已存**
（scratch 记录 + 本文），M4 开工首日销此案；诊断工具建议 ASAN/MSAN
（正好 M2 遗留清单里 ASAN 门禁排在 M6 前——此案是它的第一个客户）。

## 其他

- deopt 恢复统一路由到 vendored 循环（gen_asm 原直调 libpython
  _PyEval_EvalFrameDefault → 改 Ci_EvalFrameDefault_311，单一 micro 语义 +
  未来插桩点覆盖 deopt 帧）；
- 3.14 反向回归 0 错（frame_mode 版本条件对 3.14 无行为变化）；
- 强编 sanity：fib（递归）/loops（循环）编译执行正确。

## 工时

取料+构建收敛 ~30 分钟；SEGV 根因定位+修复 ~50 分钟（gdb 帧解剖 →
字段表比对 → 两层配置追溯）；take_ownership 修复 ~25 分钟；冒烟五件套
+ calls heisenbug 定位 ~45 分钟。**M3 预演合计 ≈ 2.5 小时**。

## 遗留（正式 M3/M4 清单）

1. **locals 物化**（M3 正式主体，复用 reifyLocalsplus）；
2. calls/MAKE_FUNCTION heisenbug（M4 首案，备 ASAN）；
3. 前端拒编清单逐项转为翻译支持（M4）；
4. auto-JIT 引导未接（M2 的 interpreter.c 直通版保留；force_compile 驱动）；
5. RuntimeTests frame/deopt 层白名单（ReifyFrameTest 等）未跑；
6. Cix_compute_cr_origin 仍是返回 None 的桩（cr_origin 默认关不触发，
   M8 前销）。
