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
post-processing fails, the pipeline stops before `cinderx_inner`. The coverage
build explicitly disables LTO with `-fno-lto`, because coverage artifacts must
be consumable by the active GCC/gcov toolchain.

Coverage artifacts are written under the run directory:

- `coverage/coverage.info`: final filtered lcov tracefile.
- `coverage/html/index.html`: browsable HTML report.
- `logs/coverage.log`: capture, filter, HTML, summary, and threshold logs.
- `summary.json`: machine-readable gate summary, including coverage metrics,
  thresholds, and coverage status.

The final report is intended to measure CinderX native project code. It filters
out third-party code, runtime test sources, test scripts, Python test packages,
build directories, `scratch`, and FetchContent `_deps` sources.

Coverage thresholds are configured in `COVERAGE_MIN_PERCENT` near the top of
`ci_pipeline/run_gate.py`. They are calibrated for the current runtime-only
coverage scope.

## Daily Lib/test Gate

The daily gate runs CPython `Lib/test` without coverage instrumentation:

```bash
python3.14 ci_pipeline/run_gate.py --suite daily
```

The `daily` suite builds a test wheel, installs it into an isolated venv, and
runs:

- `lib_test_adaptive_aware_24`: CPython `Lib/test` with the CinderX frame
  evaluator and `compile_after_n_calls(24)`, using the Kunpeng dispatcher to
  reuse workers and reduce process startup overhead.
- `lib_test_official_skip_ok_26`: a Kunpeng-only explicit run of 26 modules
  that are still present in official module-level skip metadata but passed
  under the same frame-eval/adaptive-aware mode on ARM64 CPython 3.14.

The Lib/test runner uses the official skip/JIT ignore metadata under
`cinderx/TestScripts/`, then applies the Kunpeng daily debt file
`cinderx/TestScripts/TestScriptsKunpeng/lib_test_daily_ignore_tests.txt`.
That file is kept separate from the official metadata and currently excludes
CPython internal optimizer tests that are not a CinderX frame-eval/JIT
compatibility target. The 26 additional modules are listed separately in
`cinderx/TestScripts/TestScriptsKunpeng/lib_test_daily_official_skip_ok_26.txt`;
the official skip files are not modified. The runner also removes proxy
environment variables from Lib/test subprocesses so CI proxy settings do not
change network-test behavior.

## Common Notes

Pipelines are invoked by name, for example `ci_pipeline/run_gate.py pr`.
Individual suites must be invoked with `--suite`, for example:

```bash
python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_inner
```

The test wheel enables `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1` so gate-only
package data stays out of normal release wheels.

Known exclusions:

- `test_jit_support_instrumentation.py` is filtered to ARM64-supported cases.
- `test_compiler_sbs_stdlib_0.py` through
  `test_compiler_sbs_stdlib_9.py` are tracked as Kunpeng `test_cinderx`
  debt outside the main gate. This suite is a large compiler bytecode parity
  corpus; the current performance-optimization work does not target compiler
  code generation, exception tables, or line tables. It collected 2,621 items
  on 2026-05-17, and a bounded `--maxfail=50` run stopped at
  `50 failed, 33 passed`, so it is intentionally deferred until compiler
  parity work is in scope.

Future work: when compiler parity becomes in scope, turn the SBS stdlib debt
into an explicit expected-failure or ignore baseline, then start running it
continuously.

LCOV compatibility is handled at runtime:

- LCOV 1.x uses `lcov_branch_coverage=1` and does not receive LCOV 2.x-only
  `--ignore-errors` values.
- LCOV 2.x uses `branch_coverage=1` and downgrades known third-party/template
  consistency issues during capture, filter, and HTML generation.

Keep HIR runtime test fixture files checked out with LF line endings. CRLF can
make delimiter lines fail parser validation during `runtime_tests`.
