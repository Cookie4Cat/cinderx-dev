# CinderX Local Test Gate

Chinese documentation: [README_CN.md](README_CN.md)

Local merge gates for ARM64 CPython 3.14 CinderX JIT work on top of
`meta/main`.

## PR Coverage Gate

The PR gate is intended to run with native C/C++ coverage enabled:

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

The `pr` pipeline runs the split local suites in order:

- `runtime_tests`: native RuntimeTests built and run through CMake with coverage
  instrumentation.
- `test_cinderx_release`: CinderX Python tests from the fresh non-coverage
  wheel.

Coverage mode is passed through to the `runtime` suite only. The
`cinderx_inner` suite builds and installs the normal release wheel, then runs
`test_cinderx_release`. After the `runtime` suite finishes, the gate captures
and renders GCC coverage data with `gcov`, `lcov`, and `genhtml`; if coverage
post-processing fails, the pipeline stops before `cinderx_inner`.

## Daily Compat Gate

The daily gate reuses the PR pipeline front half, then fans out compat jobs:

```bash
CINDERX_TEST_WHEEL=/path/to/cinderx.whl \
python3.14 ci_pipeline/run_gate.py daily
```

The `daily` pipeline runs in this order:

- `runtime`
- `cinderx_inner`
- `wheel_compat_<name>` entries from `ci_pipeline/python_compat_matrix.toml`
- `wheel_compat_negative_<name>` entries from the same matrix file

The compat wheel is not built by `daily`; callers must pass it in through
`CINDERX_TEST_WHEEL`. Supported and unsupported Python entries are configured in
`ci_pipeline/python_compat_matrix.toml`, and each entry must define:

- `name`
- `python`
- `version`

Each compat entry gets its own nested run directory, venv, logs, and
`summary.json`. The top-level daily summary flattens the matrix into one job per
Python version.

## Standalone Suites

Pipelines are invoked by name:

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
python3.14 ci_pipeline/run_gate.py daily
```

Individual suites must be invoked with `--suite`:

```bash
python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_inner
python3.14 ci_pipeline/run_gate.py --suite wheel_compat
python3.14 ci_pipeline/run_gate.py --suite wheel_compat_negative
```

`wheel_compat` expects:

- `CINDERX_TEST_PYTHON`
- `CINDERX_TEST_WHEEL`

`wheel_compat_negative` expects:

- `CINDERX_TEST_WHEEL`
- `CINDERX_UNSUPPORTED_TEST_PYTHON`

The test wheel enables `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1` so gate-only
package data stays out of normal release wheels.

Coverage thresholds are configured in `COVERAGE_MIN_PERCENT` near the top of
`ci_pipeline/run_gate.py`. They are calibrated for the current runtime-only
coverage scope.

Known exclusions:

- `test_jit_support_instrumentation.py` is filtered to ARM64-supported cases.
- `test_compiler_sbs_stdlib_0.py` through
  `test_compiler_sbs_stdlib_9.py` are tracked as Kunpeng `test_cinderx`
  debt outside the main gate.

LCOV compatibility is handled at runtime:

- LCOV 1.x uses `lcov_branch_coverage=1` and does not receive LCOV 2.x-only
  `--ignore-errors` values.
- LCOV 2.x uses `branch_coverage=1` and downgrades known third-party/template
  consistency issues during capture, filter, and HTML generation.

Keep HIR runtime test fixture files checked out with LF line endings. CRLF can
make delimiter lines fail parser validation during `runtime_tests`.
