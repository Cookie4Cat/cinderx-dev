from ci_pipeline.jit311.a3_census import BOOLEAN_PATHS, REQUIRED_PATHS
from ci_pipeline.jit311.a3_report import classify_blockers, judge


def _snapshot(**overrides):
    document = {
        "schema": "cp311-jit-a3-lifecycle-v1",
        "jit": {},
        "module": {},
        "runtime": {},
        "observer": {},
        "generator": {"status": "GENERATOR_NATIVE_GAUGE_NOT_AVAILABLE"},
    }
    for path in REQUIRED_PATHS:
        target = document
        parts = path.split(".")
        for part in parts[:-1]:
            target = target.setdefault(part, {})
        target[parts[-1]] = False if path in BOOLEAN_PATHS else 0
    for path, value in overrides.items():
        target = document
        parts = path.split("__")
        for part in parts[:-1]:
            target = target[part]
        target[parts[-1]] = value
    return document


def _sample(label, snapshot):
    return {
        "label": label,
        "snapshot": snapshot,
        "invariants": {"ok": True, "errors": []},
        "python_liveness": {},
        "errors": [],
    }


def test_missing_c_result_is_infrastructure_failure():
    status, _ = judge(
        prerequisite={"result": "PASS"},
        c_results={},
        ownership=None,
        finalize=None,
        command_failures=[],
    )
    assert status == "INFRA_FAIL"


def test_resident_drift_is_clustered_as_code_buffer_not_function_watch():
    baseline = _snapshot()
    final = _snapshot(runtime__resident_code_buffers=1)
    result = {
        "result": "FAIL",
        "errors": ["strict live gauges drifted: ['runtime.resident_code_buffers']"],
        "plateau": {
            "gauge_drift": {"runtime.resident_code_buffers": {"baseline": 0, "final": 1}}
        },
        "samples": [_sample("baseline", baseline), _sample("after_gc_2", final)],
    }
    blockers = classify_blockers({"C1": result}, None, None)
    assert [blocker["id"] for blocker in blockers] == ["B7"]


def test_function_liveness_signal_stays_in_function_watch_cluster():
    result = {
        "result": "FAIL",
        "errors": ["final Python liveness weakrefs_alive is non-zero"],
        "plateau": {"gauge_drift": {}},
        "samples": [_sample("baseline", _snapshot()), _sample("after_gc_2", _snapshot())],
    }
    blockers = classify_blockers({"C1": result}, None, None)
    assert [blocker["id"] for blocker in blockers] == ["B1"]


def test_refcount_failure_is_not_an_approved_deviation():
    blockers = classify_blockers({}, {"result": "FAIL", "errors": ["drift"]}, None)
    assert blockers[0]["id"] == "B10"
