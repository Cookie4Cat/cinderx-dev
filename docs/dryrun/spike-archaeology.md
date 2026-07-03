# 先期技术验证分支差异分析报告

> 分析对象：`guo/cp311-stock-cinderx-adapt`（有效变更范围 `bf6077eb..1907f234`，共三个提交）
> 用途：作为里程碑预演的参考资料。该分支与当前基线（kunpeng/dev）无共同祖先，
> 其成果只能按内容移植改写，无法直接 cherry-pick。

## 变更总量与分类

有效变更量：**94 个文件，+11,952 / −302 行**（此前的"781 文件 +47k"统计为跨谱系
diff 引入的噪音，不代表实际工作量）。

| 分类 | 文件数 | 行数 | 对应里程碑 | 说明 |
|---|---|---|---|---|
| Borrow 层 | 2 | +529 | M1 | `borrowed-3.11-fallback.c`（+504）为手工编写，未走生成管线——M1 需决策：接入生成管线，或先移植手写版本 |
| 构建系统 | 3 | +36 | M1 | 在同谱系代码上打通 3.11 构建仅需约 36 行改动，M1 的主要不确定性显著低于预估 |
| 解释器层 | 7 | +1030 | M2 | **验证分支未复制 ceval.c**（采用 stock 解释器循环 + PEP 523 frame evaluator + opcode 头文件）。因此 M2 的 vendored ceval 方案没有既有参考实现，预演中应预留充足时间 |
| JIT 层 | 39 | +5117 | M3–M7 | 主体工作：3.11 字节码到 HIR 的前端、LIR 适配、运行时辅助函数、内联缓存 |
| 测试 | 22 | +3716 | 各里程碑 | 含 test_kunpeng 下 6 个 3.11 专项测试文件，可直接移植复用 |
| 运行时接合层 | 16 | +955 | M1/M3 | PythonLib 引导、`python.h` 版本适配头、opcode 桩、Static Python 桩 |
| 其他 | 5 | +569 | — | |

## 重点文件清单（按变更量排序）

| 文件 | 行数 | 分析结论 |
|---|---|---|
| `Jit/hir/builder.cpp` | +1496 | 3.11 字节码→HIR 前端主体，M4 的主要参考实现 |
| `Jit/lir/generator.cpp` | +1080 | LIR 层适配（M4/M5） |
| `RuntimeTests/hir_test.cpp` | +1030 | 3.11 HIR 期望输出测试已有雏形，可作 M4 门禁参考 |
| `Jit/inline_cache.cpp` | +687 | **版本号守卫已有参考实现**，M7 无需从零设计 |
| `Interpreter/3.11/cinder_opcode_metadata.h` / `_ids.h` | +596/+320 | opcode 头文件（M1，可评估是否改为生成） |
| `UpstreamBorrow/borrowed-3.11-fallback.c` | +504 | 手写 borrow 回退实现 |
| `PythonLib/_cinderx_auto.py` | +423 | 自动 JIT 引导层（该分支最后一个提交修复的异常路径问题即位于此层） |
| `Jit/jit_rt.cpp` | +407 | 3.11 运行时辅助函数 |
| `StaticPython/static_python_stub.cpp` | +392 | Static Python 桩实现（范围外能力的隔离样板） |
| `Jit/hir/simplify.cpp` | +367 | HIR 简化 pass 适配 |
| `Jit/pyjit.cpp` | +256 | 初始化与入口 |
| `cinderx/python.h` | +165 | 版本适配头文件（设计书 D12 适配层的雏形） |

## 已知缺陷清单（预演中以门禁复核）

差分门禁对该验证分支构建的实测结果：918 用例中 82 项失败，基础 Lib/test 差分
26/26 模块发散。缺陷分类详见技术设计书 §8，摘要：

- 第 0 类（最高优先级）：异常处理路径存在系统性缺陷——热路径上抛出并被捕获的异常
  触发 `SystemError: error return without exception set`，标准库多处命中；
  验证分支最后一个提交即在修复此类问题，未完成；
- 第 1 类：未绑定变量守卫不完整（多处段错误、异常消息中变量名错乱）；
- 第 2 类：属性内联缓存对类型变更不失效（返回过期缓存值）；
- 第 3 类：frame/traceback/递归限制多处段错误；
- 第 4 类：CALL_FUNCTION_EX 错误消息缺少限定名前缀；
- 第 5 类：强制反优化后的恢复路径存在段错误。
