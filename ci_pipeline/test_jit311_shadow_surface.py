# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Self-tests for the MR-03 shadow-surface completeness judge.

These tests do not run regrtest or test_cinderx. They pin the reviewer's
completeness contract: the frozen 440-module list must be executed (not
imported), test_cinderx must be recorded per case, and compile_success on a
passing module cannot be satisfied by a missing worker snapshot.
"""

from __future__ import annotations

from ci_pipeline.jit311 import report as jit_report
from ci_pipeline.jit311 import runners
from ci_pipeline.jit311 import shadow_surface as surface


def _snap(**overrides):
    snap = {field: 0 for field in jit_report.RUNTIME_FIELDS}
    snap["evaluator_installed"] = True
    snap["compile_requests"] = 2
    snap["compile_success"] = 2
    snap["compile_rejected"] = 0
    snap["shadow_codegen_bytes"] = 128
    snap["peak_rss_bytes"] = 4096
    for field in jit_report.HARNESS_FIELDS:
        snap[field] = None
    snap.update(overrides)
    return snap


def _module(name: str, *, verdict: str = "pass", snap=None, cases=None, **extra):
    snap = snap if snap is not None else _snap()
    snaps = extra.pop("snapshots", [snap] if snap else [])
    record = {
        "kind": "libtest",
        "name": name,
        "verdict": verdict,
        "returncode": 0 if verdict != "crash" else 139,
        "snapshots": snaps,
        "cases": cases if cases is not None else {f"{name}.test_one": "pass"},
        "compile_success": sum(int(s.get("compile_success") or 0) for s in snaps),
    }
    record.update(extra)
    return record


def _cinderx(*, verdict: str = "fail", returncode: int = 1, snap=None, cases=None):
    snap = snap if snap is not None else _snap()
    cases = cases if cases is not None else {
        "test_cinderx.test_foo.TestFoo.test_bar": "failure",
        "test_cinderx.test_foo.TestFoo.test_ok": "pass",
    }
    return {
        "kind": "test_cinderx",
        "name": "test_cinderx",
        "verdict": verdict,
        "returncode": returncode,
        "snapshots": [snap],
        "cases": cases,
        "compile_success": snap.get("compile_success", 0),
    }


def _report(modules, cinderx=None, **extra):
    report = {
        "frozen_module_count": extra.pop("frozen_module_count", 440),
        "libtest_modules": modules,
        "libtest_worker_crashes": extra.pop(
            "libtest_worker_crashes",
            sum(1 for rec in modules if rec["verdict"] in ("crash", "no_result")),
        ),
        "test_cinderx": cinderx if cinderx is not None else _cinderx(),
    }
    report.update(extra)
    return report


def _full_surface():
    return _report([_module(f"test_{i}") for i in range(440)])


def test_sitecustomize_is_valid_python():
    compile(surface.SURFACE_SITECUSTOMIZE, "sitecustomize.py", "exec")
    assert "ModuleNotFoundError" in surface.SURFACE_SITECUSTOMIZE
    assert "JIT311_SURFACE_MODULE" in surface.SURFACE_SITECUSTOMIZE
    assert "JIT311_CINDERX_SITE" in surface.SURFACE_SITECUSTOMIZE
    assert "_evaluator_installed_at_start" in surface.SURFACE_SITECUSTOMIZE


def test_stdlib_import_canary_is_still_72():
    assert len(runners.STDLIB_SHADOW_MODULES) == 72
    assert len(set(runners.STDLIB_SHADOW_MODULES)) == 72
    doc = runners.stdlib_shadow_runner.__doc__ or ""
    assert "canary" in doc.lower() or "MR-05" in doc


def test_frozen_surface_is_440():
    modules = runners.load_libtest_target_manifest()
    assert len(modules) == 440
    assert surface.load_frozen_modules() == modules


def test_healthy_surface_is_green():
    assert surface.judge_completeness(_full_surface()) == []


def test_import_only_style_zero_compile_on_executed_module_is_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_int",
            snap=_snap(compile_requests=0, compile_success=0, compile_rejected=0),
        )
    )
    errors = surface.judge_completeness(_report(modules))
    assert any("compile_success=0" in err and "test_int" in err for err in errors)


def test_skip_without_compile_is_green():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(
        _module(
            "test_windows",
            verdict="skip",
            cases={"test_windows.test_one": "skipped"},
            snap=_snap(compile_requests=0, compile_success=0, compile_rejected=0),
        )
    )
    assert surface.judge_completeness(_report(modules)) == []


def test_crash_and_missing_snapshot_are_red():
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(_module("test_crash", verdict="crash", snapshots=[], cases={}))
    errors = surface.judge_completeness(_report(modules, libtest_worker_crashes=1))
    assert any("libtest_worker_crashes=1" in err for err in errors)
    assert any("test_crash" in err and "crash" in err for err in errors)
    assert any("no worker snapshots" in err for err in errors)


def test_unknown_rejects_and_machine_code_are_red():
    bad = _snap(
        unknown_rejects=1, compile_rejected=1, compile_success=1, compile_requests=2
    )
    modules = [_module(f"test_{i}") for i in range(439)]
    modules.append(_module("test_bad", snap=bad))
    errors = surface.judge_completeness(_report(modules))
    assert any("unknown_rejects" in err for err in errors)

    leaked = _snap(machine_code_entries=1)
    modules[-1] = _module("test_bad", snap=leaked)
    errors = surface.judge_completeness(_report(modules))
    assert any("machine_code_entries" in err for err in errors)


def test_shrunk_surface_is_red():
    errors = surface.judge_completeness(_report([_module("test_int")]))
    assert any("440" in err for err in errors)


def test_missing_test_cinderx_is_red():
    report = _full_surface()
    report["test_cinderx"] = None
    errors = surface.judge_completeness(report)
    assert any("test_cinderx completeness record is missing" in err for err in errors)


def test_test_cinderx_pytest_failures_are_recorded_not_red():
    # Shadow cannot execute installed machine code; tests that assume JIT
    # execution fail. Completeness still holds if the suite finished and
    # compiled real test functions.
    assert surface.judge_completeness(_full_surface()) == []


def test_test_cinderx_collection_errors_are_not_abnormal_exits():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(verdict="fail", returncode=2)
    assert surface.judge_completeness(report) == []


def test_test_cinderx_worker_crash_is_red():
    report = _full_surface()
    report["test_cinderx"] = _cinderx(verdict="crash", returncode=-11)
    errors = surface.judge_completeness(report)
    assert any("crash" in err for err in errors)


def test_pytest_process_verdict():
    assert surface.pytest_process_verdict(0, {"a": "pass"}, False) == "pass"
    assert surface.pytest_process_verdict(1, {"a": "failure"}, False) == "fail"
    assert surface.pytest_process_verdict(2, {}, False) == "fail"
    assert surface.pytest_process_verdict(5, {}, False) == "no_result"
    assert surface.pytest_process_verdict(0, {}, True) == "crash"


def test_test_cinderx_suites_come_from_the_official_runner():
    suites = surface.load_test_cinderx_suites()
    names = [suite["name"] for suite in suites]
    assert "all_test_cinderx" in names
    assert "test_compiler" in names


def test_subset_does_not_claim_the_frozen_bar():
    report = _report(
        [_module("test_int")],
        frozen_module_count=1,
        test_cinderx=_cinderx(),
    )
    assert surface.judge_completeness(report, require_frozen=False) == []
    assert surface.judge_completeness(report, require_frozen=True)


def test_snapshot_extras_are_stripped_before_schema():
    snap = _snap()
    snap["surface_module"] = "test_int"
    snap["surface_kind"] = "libtest"
    assert surface.snapshot_errors(snap) == []


def test_sequential_regrtest_success_is_pass_not_no_result():
    # 3.11.6 `python -m test test_int` (no -j) prints the module name
    # without "passed"; the epilogue plus junit is the verdict.
    log = (
        "0:00:00 load avg: 0.16 Run tests sequentially (timeout: 3 min)\n"
        "0:00:00 load avg: 0.16 [1/1] test_int\n"
        "\n"
        "== Tests result: SUCCESS ==\n"
        "\n"
        "1 test OK.\n"
        "Result: SUCCESS\n"
    )
    cases = {"test.test_int.IntTestCases.test_bit_length": "pass"}
    assert (
        surface.infer_libtest_verdict("test_int", log, 0, cases, False) == "pass"
    )
    assert (
        surface.infer_libtest_verdict("test_int", log, 0, {}, False) == "pass"
    )
