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


def test_skip_is_not_a_regression():
    stock = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "pass"})
    cinderx = _arm({"test_x": "pass"}, {"test.test_x.T.test_a": "skipped"})
    diff = libtest_diff.diff_results(stock, cinderx)
    assert diff["case_regressions"] == {}
    assert diff["module_regressions"] == {}


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
