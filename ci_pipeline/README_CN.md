# CinderX 本地测试门禁

英文文档: [README.md](README.md)

这是用于 ARM64 CPython 3.14 CinderX JIT 工作流的本地合入门禁，基于
`meta/main`。

## PR 覆盖率门禁

PR 门禁推荐以 native C/C++ 覆盖率模式运行：

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

`pr` suite 会构建测试 wheel，安装到隔离 venv，然后运行：

- `runtime_tests`: 使用 CMake 构建并运行带覆盖率插桩的 native RuntimeTests。
- `test_cinderx_release`: 使用新构建的非覆盖率 wheel 运行 CinderX Python 测试。

覆盖率模式只会对明确参与覆盖率的 job 插桩。当前只有 `runtime_tests`；
`setup_release` 和 `test_cinderx_release` 保持普通 release wheel 路径。suite
jobs 结束后，门禁会使用 `gcov`、`lcov` 和 `genhtml` 采集并生成 GCC 覆盖率报告。
覆盖率构建会显式禁用 LTO，因为覆盖率产物需要由当前 GCC/gcov 工具链读取。

覆盖率产物会写入本次运行目录：

- `coverage/coverage.info`: 最终过滤后的 lcov tracefile。
- `coverage/html/index.html`: 可在浏览器中查看的 HTML 报告。
- `logs/coverage.log`: capture、filter、HTML、summary 和阈值检查日志。
- `summary.json`: 面向脚本读取的门禁摘要，包含覆盖率指标、阈值和状态。

最终报告用于统计 CinderX native 项目代码。报告会过滤第三方代码、runtime test
源码、测试脚本、Python 测试包、构建目录、`scratch` 和 FetchContent `_deps`
源码。

覆盖率阈值配置在 `ci_pipeline/run_gate.py` 顶部附近的
`COVERAGE_MIN_PERCENT`，当前按 runtime-only 覆盖率范围校准。

## Daily Lib/test 门禁

日构建门禁运行不带覆盖率插桩的 CPython `Lib/test`：

```bash
python3.14 ci_pipeline/run_gate.py daily
```

`daily` suite 会构建测试 wheel，安装到隔离 venv，然后运行：

- `lib_test_adaptive_aware_24`: 使用 CinderX frame evaluator 和
  `compile_after_n_calls(24)` 运行 CPython `Lib/test`，并通过 Kunpeng
  dispatcher 复用 worker，降低进程启动开销。

Lib/test runner 会先使用 `cinderx/TestScripts/` 下的官方 skip/JIT ignore
metadata，然后追加 Kunpeng daily 债务文件
`cinderx/TestScripts/TestScriptsKunpeng/lib_test_daily_ignore_tests.txt`。
这个文件和官方 metadata 分开维护，当前只用于排除不属于 CinderX
frame-eval/JIT 兼容性目标的 CPython 内部 optimizer 测试。runner 还会从
Lib/test 子进程环境中清理代理变量，避免 CI 代理设置影响网络相关测试行为。

## 通用说明

测试 wheel 会启用 `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1`，从而避免门禁专用的
package data 进入普通发布版本 wheel。

已知排除：

- `test_jit_support_instrumentation.py` 仅运行 ARM64 支持的用例。

后续工作：增加 `test_compiler_sbs_stdlib_0.py` 到
`test_compiler_sbs_stdlib_9.py` 的 compiler side-by-side 覆盖。

LCOV 兼容逻辑会在运行时根据版本选择参数：

- LCOV 1.x 使用 `lcov_branch_coverage=1`，并且不会传入 LCOV 2.x 才支持的
  `--ignore-errors` 值。
- LCOV 2.x 使用 `branch_coverage=1`，并在 capture、filter 和 HTML 生成阶段
  将已知第三方/template 一致性问题降级处理。

HIR runtime test fixture 文件需要保持 LF 换行。CRLF 会导致 delimiter 行在
`runtime_tests` 阶段解析校验失败。
