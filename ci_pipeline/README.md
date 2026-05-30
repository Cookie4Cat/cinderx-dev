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
`cinderx_local` suite builds and installs the normal release wheel, then runs
`test_cinderx_release`. After the `runtime` suite finishes, the gate captures
and renders GCC coverage data with `gcov`, `lcov`, and `genhtml`; if coverage
post-processing fails, the pipeline stops before `cinderx_local`.

## Daily Compat Gate

The daily gate reuses the PR pipeline front half, then fans out compat jobs:

```bash
CINDERX_TEST_WHEEL=/path/to/cinderx.whl \
python3.14 ci_pipeline/run_gate.py daily
```

The `daily` pipeline runs in this order:

- `runtime`
- `cinderx_local` with local-wheel Lib/test enabled
- `wheel_compat_<name>` entries from `ci_pipeline/python_compat_matrix.toml`
- `wheel_compat_negative_<name>` entries from the same matrix file

The `daily` pipeline runs Lib/test against the locally built release wheel by
enabling `CINDERX_LOCAL_RUN_LIBTEST` for `cinderx_local`. The external compat
wheel is not built by `daily`; callers must pass it in through
`CINDERX_TEST_WHEEL`. Supported and unsupported Python entries are configured
in `ci_pipeline/python_compat_matrix.toml`, and each entry must define:

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
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
python3.14 ci_pipeline/run_gate.py --suite wheel_compat
python3.14 ci_pipeline/run_gate.py --suite wheel_compat_negative
```

`cinderx_local` normally runs only the local release wheel build and CinderX
Python tests. Set `CINDERX_LOCAL_RUN_LIBTEST=1` to include the local-wheel
Lib/test jobs, which is what the `daily` pipeline does.

For offline ARM64 hosts, pass dependency paths through the environment before
running a pipeline or standalone suite:

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1

python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
```

Leave `CINDERX_LOCAL_RUN_LIBTEST` unset when running the ordinary
`cinderx_local` suite without Lib/test.

`wheel_compat` expects:

- `CINDERX_TEST_PYTHON`
- `CINDERX_TEST_WHEEL`

`wheel_compat_negative` expects:

- `CINDERX_TEST_WHEEL`
- `CINDERX_UNSUPPORTED_TEST_PYTHON`

The test wheel enables `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1` so gate-only
package data stays out of normal release wheels.

## Fat Wheel Build Options

The CPython 3.14 manylinux fat wheel builder defaults to release-oriented
settings:

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py
```

Default build behavior:

- `CMAKE_BUILD_TYPE=Release`
- `CINDERX_ENABLE_PGO=0`
- `CINDERX_ENABLE_LTO=0`

Use explicit flags when a CI job needs different build characteristics:

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py \
  --cmake-build-type RelWithDebInfo \
  --pgo \
  --lto
```

The host build manifest records the selected CMake build type and whether PGO
or LTO was enabled. The in-container build script follows the same defaults and
only enables PGO/LTO when `CINDERX_ENABLE_PGO` or `CINDERX_ENABLE_LTO` is set to
a non-zero value.

## Dependency Cache And Pip Offline Mode

`run_gate` can pass a CMake FetchContent dependency cache into RuntimeTests and
wheel builds. Local suites do not hard-code a cache path; set one explicitly
when the host should use offline or pre-populated dependencies:

- `CINDERX_LOCAL_DEPS`: local cache directory for CMake FetchContent
  dependencies

The cache covers:

- `fmt`
- `parallel-hashmap`
- `usdt`
- `capstone`
- `googletest`

If a cached dependency is missing or does not match the expected remote and
tag/commit, CMake refreshes that dependency in the cache.

For Python package bootstrap in suite venvs, set:

- `CINDERX_PIP_WHEELHOUSE`: local wheelhouse containing `pip`, `pytest`, and
  pytest's transitive dependencies
- `CINDERX_PIP_OFFLINE=1`: require `pip` installs to use the local wheelhouse
  only

Example:

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1

python3.14 ci_pipeline/run_gate.py pr --coverage
```

Without `CINDERX_LOCAL_DEPS`, CMake falls back to the normal FetchContent
behavior for the current environment.

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
