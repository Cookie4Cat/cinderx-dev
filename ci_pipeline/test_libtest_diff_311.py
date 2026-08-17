"""Regression semantics of the 3.11 Lib/test differential engine.

The engine's contract is regression-only red: anything that passed on the
stock arm and no longer passes on the CinderX arm is a regression, at the
module level and at the case level -- including a case that silently
vanishes from a module that otherwise still reports.
"""

import ci_pipeline.libtest_diff_311 as libtest_diff


def _arm(modules, cases):
    return {"modules": modules, "cases": cases}


def test_case_failure_is_a_regression():
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_a": {"stock": "pass", "cinderx": "failure"}
    }
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "fail"}
    }


def test_case_vanishing_from_a_reporting_module_is_a_regression():
    stock = _arm(
        {"test_x": "pass"},
        {"test.test_x.T.test_a": "pass", "test.test_x.T.test_b": "pass"},
    )
    cinderx = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_b": {"stock": "pass", "cinderx": "missing"}
    }


def test_dead_module_reports_once_at_module_level_not_per_case():
    stock = _arm(
        {"test_x": "pass"},
        {"test.test_x.T.test_a": "pass", "test.test_x.T.test_b": "pass"},
    )
    cinderx = _arm({"test_x": "no_result"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "no_result"}
    }
    assert diff["case_regressions"] == {}


def test_case_pass_to_skip_is_a_regression():
    # A case that starts skipping under the evaluator shrank coverage
    # while staying green -- same rule as the module level.
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "skipped"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {
        "test.test_x.T.test_a": {"stock": "pass", "cinderx": "skipped"}
    }
    assert diff["module_regressions"] == {}


def test_crashed_worker_keeps_a_distinct_verdict():
    # 3.11.6 regrtest prints "<name> process crashed" (libregrtest/
    # runtest.py); the bare form is kept for robustness.
    log = "0:00:01 load avg: 1.0 [1/2] test_x process crashed (SIGSEGV)\n" \
          "0:00:01 load avg: 1.0 [2/2] test_y crashed"
    verdicts = libtest_diff.parse_regrtest_modules(log, ["test_x", "test_y"])
    assert verdicts == {"test_x": "crash", "test_y": "crash"}
    assert libtest_diff.crash_count(verdicts) == 2


def test_symmetric_crash_cannot_launder_to_baseline():
    # Two arms crashing identically must never read as green: the verdict
    # stays "crash" (not "fail"), the arm-level crash gate fires before
    # any diff, and an asymmetric crash is a regression in the diff too.
    stock = _arm({"test_x": "pass"}, {})
    cinderx = _arm({"test_x": "crash"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "crash"}
    }


def test_stock_failures_are_baseline_not_regressions():
    stock = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    cinderx = _arm({"test_x": "fail"}, {"test.test_x.T.test_a": "failure"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {}
    assert diff["module_regressions"] == {}


def test_module_verdicts_come_from_regrtest_lines():
    log = (
        "0:00:00 load avg: 7.78 [  1/440] test_a passed\n"
        "0:00:00 load avg: 7.78 [  2/440/1] test_b failed (uncaught exception)\n"
        "test test_b crashed -- Traceback (most recent call last):\n"
        "0:00:01 load avg: 7.78 [  3/440/1] test_c skipped (resource denied)\n"
        "0:00:01 load avg: 7.78 [  4/440/1] test_d skipped\n"
        "0:00:01 load avg: 7.78 [  5/440/1] test.test_pkg.test_sub passed\n"
        "0:00:02 load avg: 7.78 [  6/440/1] test_unrequested passed\n"
        "0:00:02 load avg: 7.78 [  7/440/2] test_e failed (env changed)\n"
    )
    requested = ["test_a", "test_b", "test_c", "test_d",
                 "test.test_pkg.test_sub", "test_e", "test_worker_died"]
    verdicts = libtest_diff.parse_regrtest_modules(log, requested)
    assert verdicts == {
        "test_a": "pass",
        "test_b": "fail",
        "test_c": "skip",
        "test_d": "skip",
        "test.test_pkg.test_sub": "pass",
        "test_e": "fail",
    }
    # A requested module without a result line stays absent: the caller
    # records it as no_result, the only verdict left for a dead worker.
    assert "test_worker_died" not in verdicts
    # Unrequested names (phantom keys) never enter the accounting.
    assert "test_unrequested" not in verdicts


def test_pass_to_skip_is_a_regression():
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "skip"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {
        "test_x": {"stock": "pass", "cinderx": "skip"}
    }
    # The module-level entry carries the signal; no per-case spam.
    assert diff["case_regressions"] == {}


def test_symmetric_skip_and_fail_are_baseline():
    stock = _arm({"test_x": "skip", "test_y": "fail"}, {})
    cinderx = _arm({"test_x": "skip", "test_y": "fail"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["module_regressions"] == {}
    assert diff["module_warnings"] == {}


def test_attest_reader(tmp_path):
    attest = tmp_path / "attest.log"
    count, ok = libtest_diff.read_attest(attest)
    assert (count, ok) == (0, True)

    attest.write_text("101 True\n102 True\n103 True\n")
    assert libtest_diff.read_attest(attest) == (3, True)

    attest.write_text("101 True\n102 False\n")
    assert libtest_diff.read_attest(attest) == (2, False)


def test_startup_sitecustomize_compiles():
    compile(libtest_diff.STARTUP_SITECUSTOMIZE, "sitecustomize.py", "exec")


def test_missing_verdicts_flags_only_unreported_modules():
    # pass/fail/skip are all verdicts; only no_result means the module was
    # never reported at all, which must fail the arm rather than flow into
    # a possibly-symmetric (false green) diff.
    assert libtest_diff.missing_verdicts(
        {"a": "pass", "b": "fail", "c": "skip"}
    ) == 0
    assert libtest_diff.missing_verdicts(
        {"a": "pass", "b": "no_result", "c": "no_result"}
    ) == 2


def test_package_path_cases_resolve_to_their_requested_module():
    stock = _arm(
        {"test.test_pkg.test_sub": "pass"},
        {"test.test_pkg.test_sub.T.test_a": "pass"},
    )
    cinderx = _arm({"test.test_pkg.test_sub": "no_result"}, {})
    diff = libtest_diff.diff_results(stock, cinderx)
    # The vanished case maps back onto the dead package module and is
    # suppressed in favor of the module-level entry.
    assert diff["case_regressions"] == {}
    assert "test.test_pkg.test_sub" in diff["module_regressions"]


def test_arm_environment_is_sanitized():
    # An inherited PYTHONPATH / sitecustomize hook / CINDERX_* variable
    # could activate machinery in BOTH arms and fake a neutral diff.
    dirty = {
        "PATH": "/bin",
        "PYTHONPATH": "/somewhere/evil",
        "PYTHONSTARTUP": "/evil.py",
        "PYTHONHOME": "/opt/py",
        "PYTHONHASHSEED": "random",
        "PYTHONJITAUTO": "4",
        "PARALLEL_GC_THRESHOLD": "9",
        "CINDERX_DIFF_ATTEST": "/tmp/x",
        "CINDERX_JIT_MODE": "shadow",
    }
    env = libtest_diff.arm_environment(dirty)
    # The inherited random hash seed is REPLACED, not merely defaulted.
    assert env == {"PATH": "/bin", "PYTHONHASHSEED": "0"}, env


def test_stock_startup_attests_purity(tmp_path):
    import subprocess
    import sys as _sys

    startup = tmp_path / "startup"
    startup.mkdir()
    (startup / "sitecustomize.py").write_text(libtest_diff.STOCK_SITECUSTOMIZE)
    ledger = tmp_path / "ledger.log"
    base_env = {
        "PYTHONPATH": str(startup),
        "CINDERX_STOCK_ATTEST": str(ledger),
        "PATH": "/usr/bin:/bin",
    }

    subprocess.run([_sys.executable, "-c", "pass"], env=base_env, check=True)
    subprocess.run(
        [_sys.executable, "-c", "import sys; sys.modules['cinderx'] = sys"],
        env=base_env, check=True,
    )
    count, clean = libtest_diff.read_stock_attest(ledger)
    assert count == 2 and not clean
    lines = ledger.read_text().splitlines()
    assert lines[0] == "clean"
    assert lines[1] == "POLLUTED:cinderx"


def test_arm_completion_discipline():
    log_full = (
        "0:00:01 load avg: 1.0 [1/2] test_a passed\n"
        "0:00:01 load avg: 1.0 [2/2] test_b passed\n"
        "== Tests result: SUCCESS ==\n"
    )
    assert libtest_diff.arm_run_completed(0, log_full) is None
    assert libtest_diff.arm_run_completed(2, log_full) is None
    assert libtest_diff.arm_run_completed(3, log_full) is None
    # Full per-module verdicts plus a signal death: the verdict lines must
    # not certify a run whose main process was killed.
    err = libtest_diff.arm_run_completed(-9, log_full)
    assert err and "abnormally" in err
    assert libtest_diff.arm_run_completed(130, log_full)
    assert libtest_diff.arm_run_completed(1, log_full)
    # A normal-looking exit code without the completion epilogue is a
    # truncated run, not a completed one.
    assert libtest_diff.arm_run_completed(
        0, "0:00:01 load avg: 1.0 [1/1] test_a passed\n"
    )


def test_worker_thread_failure_cannot_be_certified():
    # 3.11.6 regrtest reports an internal worker-thread death AFTER the
    # module verdicts, then still prints a normal FAILURE epilogue and
    # exits 2 -- verdict parsing alone would certify the run and publish
    # worker_crashes=0.  The real output shape must be refused.
    log = (
        "0:00:01 load avg: 1.0 [1/2] test_a passed\n"
        "0:00:01 load avg: 1.0 [2/2] test_b passed\n"
        "regrtest worker thread failed: Traceback (most recent call last):\n"
        '  File "/usr/lib64/python3.11/test/libregrtest/runtest_mp.py"\n'
        "== Tests result: FAILURE ==\n"
    )
    err = libtest_diff.arm_run_completed(2, log)
    assert err and "harness-internal" in err
