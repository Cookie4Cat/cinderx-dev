from ci_pipeline.jit311.a3_census import BOOLEAN_PATHS, REQUIRED_PATHS
from ci_pipeline.jit311.a3_core import apply_core_policy, match_sensitive


def _snapshot(**overrides):
    document = {
        "schema": "cp311-jit-a3-lifecycle-v1",
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


def _c1_result(*, live=True, deaths=100, capacity_error=True):
    samples = [_sample("baseline", _snapshot())]
    if live:
        samples.append(
            _sample(
                "live_1",
                _snapshot(
                    jit__installed_functions=1, jit__watched_functions=1
                ),
            )
        )
    samples.append(
        _sample(
            "after_gc_2",
            _snapshot(runtime__function_destroyed_notifications=deaths),
        )
    )
    errors = []
    if capacity_error:
        errors.append(
            "capacity jit.code_runtimes_allocated grows from after_10=11 to "
            "after_100=101"
        )
    return {
        "scenario": "C1",
        "result": "FAIL" if errors else "PASS",
        "cycles": [1, 10, 100],
        "samples": samples,
        "plateau": {"capacity": {"jit.code_runtimes_allocated": {"plateau": False}}},
        "errors": errors,
    }


def test_capacity_errors_become_diagnostics_not_verdicts():
    result = apply_core_policy(_c1_result())
    assert result["result"] == "PASS"
    assert result["errors"] == []
    demoted = result["resource_stability_diagnostic"]["errors_demoted"]
    assert any("code_runtimes_allocated" in entry for entry in demoted)


def test_vacuous_run_fails_even_with_clean_gauges():
    # A scenario whose populations never rose must not pass on empty
    # invariants: the live_1 rise and the cumulative movement are both
    # required (v1.1 §10).
    silent = _c1_result(live=False, deaths=0, capacity_error=False)
    result = apply_core_policy(silent)
    assert result["result"] == "FAIL"
    assert any("live_1" in error for error in result["errors"])

    lazy = _c1_result(deaths=3, capacity_error=False)
    result = apply_core_policy(lazy)
    assert result["result"] == "FAIL"
    assert any("function_destroyed_notifications" in e for e in result["errors"])


def test_real_errors_survive_the_demotion():
    result = _c1_result(capacity_error=False)
    result["errors"] = ["strict live gauges drifted: ['jit.watched_functions']"]
    result["result"] = "FAIL"
    judged = apply_core_policy(result)
    assert judged["result"] == "FAIL"
    assert judged["errors"][0].startswith("strict live gauges drifted")


BROAD_PATTERNS = [
    "cinderx/Jit/**",
    "cinderx/Common/**",
    "cinderx/Interpreter/**",
    "cinderx/module_state.h",
    "ci_pipeline/jit311/a3_census.py",
]


def test_trigger_covers_the_files_the_lifecycle_fixes_touched():
    # The v1.1 draft's narrow list missed exactly these; the shipped broad
    # list must not.
    touched = [
        "cinderx/Jit/gen_data_footer.h",
        "cinderx/Jit/jit_rt.cpp",
        "cinderx/Common/slab_arena.h",
        "cinderx/Jit/context_iface.h",
        "cinderx/Common/code_extra.cpp",
        "cinderx/Interpreter/3.11/observe.c",
    ]
    assert match_sensitive(touched, BROAD_PATTERNS) == touched


def test_trigger_ignores_unrelated_paths():
    changed = ["docs/design/whatever.md", "ci_pipeline/test_a3_core.py"]
    assert match_sensitive(changed, BROAD_PATTERNS) == []
