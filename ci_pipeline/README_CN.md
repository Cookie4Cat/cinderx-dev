# CinderX 本地测试门禁

英文文档: [README.md](README.md)

这是用于 ARM64 CPython 3.14 CinderX JIT 工作流的本地合入门禁，基于
`meta/main`。

```bash
python3.14 ci_pipeline/run_gate.py pr
```

在 ARM64 服务器上，进入代码仓后使用目标 CPython 3.14 解释器执行同一命令。
日志和 `summary.json` 会写入 `build/testgate/`。

`pr` 套件会构建测试 wheel，安装到隔离的 venv 中，并且当前会运行:

- `test_cinderx_release`: 使用新构建 wheel 运行 CinderX Python 测试。

测试 wheel 会启用 `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1`，从而避免门禁专用的
package data 进入普通发行版 wheel。

已知排除:

- `test_jit_support_instrumentation.py` 仅运行 ARM64 支持的用例。

后续工作: 增加 `test_compiler_sbs_stdlib_0.py` 到
`test_compiler_sbs_stdlib_9.py` 的 compiler side-by-side 覆盖。

## 覆盖率门禁

使用下面的命令在同一套 `pr` 门禁上开启 native C/C++ 覆盖率:

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

覆盖率模式会先运行普通 `pr` jobs，然后使用 `gcov`、`lcov` 和 `genhtml`
采集并生成 GCC 覆盖率报告。覆盖率构建会显式通过 `-fno-lto` 禁用 LTO，
因为覆盖率产物需要由当前 GCC/gcov 工具链读取。

覆盖率产物会写入本次运行目录:

- `coverage/coverage.info`: 最终过滤后的 lcov tracefile。
- `coverage/html/index.html`: 可在浏览器中查看的 HTML 报告。
- `logs/coverage.log`: capture、filter、HTML、summary 和阈值检查日志。
- `summary.json`: 面向脚本读取的门禁摘要，包含 `coverage.metrics`、
  `coverage.thresholds` 和覆盖率状态。

最终报告用于统计 CinderX native 项目代码。报告会过滤第三方代码、
runtime test 源码、测试脚本、Python 测试包、构建目录、`scratch` 和
FetchContent `_deps` 源码。

覆盖率门禁会根据 `ci_pipeline/run_gate.py` 顶部附近的
`COVERAGE_MIN_PERCENT` 配置检查行、函数和分支覆盖率。如果任一指标低于
配置阈值，coverage 步骤会失败，整体门禁返回非 0。

`summary.json` 中的覆盖率指标来自最终过滤后的 `coverage.info` tracefile，
不是 `coverage.log` 中最先打印的原始 summary。

LCOV 兼容逻辑会在运行时根据版本选择参数:

- LCOV 1.x 使用 `lcov_branch_coverage=1`，并且不会传入 LCOV 2.x 才支持的
  `--ignore-errors` 值。
- LCOV 2.x 使用 `branch_coverage=1`，并在 capture、filter 和 HTML 生成阶段
  将已知第三方/template 一致性问题降级处理。

HIR runtime test fixture 文件需要保持 LF 换行。CRLF 会导致 delimiter 行在
`runtime_tests` 阶段解析校验失败。
