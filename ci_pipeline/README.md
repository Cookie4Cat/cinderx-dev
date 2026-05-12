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

## Coverage Gate

Run the same suite with native C/C++ coverage enabled:

```bash
python3.14 ci_pipeline/run_gate.py pr --coverage
```

Coverage mode runs the normal `pr` jobs first, then captures and renders GCC
coverage data with `gcov`, `lcov`, and `genhtml`. The coverage build explicitly
disables LTO with `-fno-lto`, because coverage artifacts must be consumable by
the active GCC/gcov toolchain.

Coverage artifacts are written under the run directory:

- `coverage/coverage.info`: final filtered lcov tracefile.
- `coverage/html/index.html`: browsable HTML report.
- `logs/coverage.log`: capture, filter, HTML, summary, and threshold logs.
- `summary.json`: machine-readable gate summary, including
  `coverage.metrics`, `coverage.thresholds`, and coverage status.

The final report is intended to measure CinderX native project code. It filters
out third-party code, runtime test sources, test scripts, Python test packages,
build directories, `scratch`, and FetchContent `_deps` sources.

The coverage gate enforces line, function, and branch coverage thresholds from
`COVERAGE_MIN_PERCENT` near the top of `ci_pipeline/run_gate.py`. If any metric
falls below the configured minimum, the coverage step fails and the overall gate
returns a non-zero exit code.

`summary.json` coverage metrics are parsed from the final filtered
`coverage.info` tracefile, not from the first summary printed in
`coverage.log`.

LCOV compatibility is handled at runtime:

- LCOV 1.x uses `lcov_branch_coverage=1` and does not receive LCOV 2.x-only
  `--ignore-errors` values.
- LCOV 2.x uses `branch_coverage=1` and downgrades known third-party/template
  consistency issues during capture, filter, and HTML generation.

Keep HIR runtime test fixture files checked out with LF line endings. CRLF can
make delimiter lines fail parser validation during `runtime_tests`.
