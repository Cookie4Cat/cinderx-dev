# CinderX Local Test Gate

Chinese documentation: [README_CN.md](README_CN.md)

Local merge gate for ARM64 CPython 3.14 CinderX JIT work on top of
`meta/main`.

```bash
python3.14 ci_pipeline/run_gate.py pr
```

On the ARM64 server, run the same command inside the checkout, using the desired
CPython 3.14 interpreter. Logs and `summary.json` are written under
`build/testgate/`.

The `pr` suite builds a test wheel, installs it into an isolated venv, and
currently runs:

- `test_cinderx_release`: CinderX Python tests from the fresh wheel.

The test wheel enables `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1` so gate-only
package data stays out of normal release wheels.

Known exclusions:

- `test_jit_support_instrumentation.py` is filtered to ARM64-supported cases.

Future work: add compiler side-by-side coverage for
`test_compiler_sbs_stdlib_0.py` through `test_compiler_sbs_stdlib_9.py`.
