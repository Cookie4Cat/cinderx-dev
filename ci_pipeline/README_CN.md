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
