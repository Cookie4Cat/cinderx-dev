# CinderX 本地测试门禁

英文文档：[README.md](README.md)

这里维护 ARM64 CPython 3.14 CinderX JIT 的本地门禁流程。

## PR 覆盖率门禁

PR 门禁推荐带 native C/C++ 覆盖率运行：

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

`pr` pipeline 按顺序运行：

- `runtime_tests`：使用 CMake 构建并运行带覆盖率插桩的 native RuntimeTests
- `test_cinderx_release`：使用新构建的非覆盖率 release wheel 运行 CinderX Python 测试

覆盖率只透传给 `runtime` suite。`cinderx_inner` 负责构建并安装普通 release
wheel，然后运行 `test_cinderx_release`。`runtime` 结束后会立即执行 `gcov`、
`lcov`、`genhtml` 的覆盖率后处理；如果覆盖率后处理失败，pipeline 会在进入
`cinderx_inner` 前停止。

## Daily 兼容性门禁

`daily` 会复用 `pr` 的前半段，然后展开多 Python 兼容性验证：

```bash
CINDERX_TEST_WHEEL=/path/to/cinderx.whl \
python3.14 ci_pipeline/run_gate.py daily
```

`daily` 的执行顺序是：

- `runtime`
- `cinderx_inner`
- `ci_pipeline/python_compat_matrix.toml` 中的 `wheel_compat_<name>`
- 同一文件中的 `wheel_compat_negative_<name>`

`daily` 不负责构建 compat wheel，调用方必须通过 `CINDERX_TEST_WHEEL` 传入。
支持与不支持的 Python 版本统一配置在
`ci_pipeline/python_compat_matrix.toml`，每个条目必须显式包含：

- `name`
- `python`
- `version`

每个 compat 条目都会生成独立的子目录、venv、日志和 `summary.json`。顶层
`daily` summary 会把每个 Python 版本直接作为一个独立 job 展示。

## 独立 Suite

pipeline 通过名称调用：

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
python3.14 ci_pipeline/run_gate.py daily
```

单独运行 suite 必须使用 `--suite`：

```bash
python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_inner
python3.14 ci_pipeline/run_gate.py --suite wheel_compat
python3.14 ci_pipeline/run_gate.py --suite wheel_compat_negative
```

`wheel_compat` 需要：

- `CINDERX_TEST_PYTHON`
- `CINDERX_TEST_WHEEL`

`wheel_compat_negative` 需要：

- `CINDERX_TEST_WHEEL`
- `CINDERX_UNSUPPORTED_TEST_PYTHON`

测试 wheel 会启用 `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1`，避免门禁专用 package
data 进入普通发布 wheel。

覆盖率阈值定义在 `ci_pipeline/run_gate.py` 顶部附近的
`COVERAGE_MIN_PERCENT`，当前按 runtime-only 覆盖范围校准。

已知排除项：

- `test_jit_support_instrumentation.py` 仅运行 ARM64 支持的用例
- `test_compiler_sbs_stdlib_0.py` 到 `test_compiler_sbs_stdlib_9.py` 仍作为
  Kunpeng `test_cinderx` 债务，暂不纳入主门禁

LCOV 兼容逻辑会在运行时按版本自动选择参数：

- LCOV 1.x 使用 `lcov_branch_coverage=1`
- LCOV 2.x 使用 `branch_coverage=1`

请保持 HIR runtime test fixture 文件使用 LF 换行；CRLF 会导致
`runtime_tests` 里的分隔符校验失败。
