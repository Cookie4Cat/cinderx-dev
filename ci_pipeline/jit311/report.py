# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Unified trigger-proof run report for the CPython 3.11 JIT gates.

Produces the JSON document defined in docs/cp311-jit-dev-plan.md (MR-01).
Every field is either measured from the running process (runtime-derived
fields) or owned by the harness that orchestrates worker processes
(harness-owned fields).  Runtime-derived fields come from read-only
introspection only: nothing here mutates JIT state, so a report can be taken
at any point without perturbing the run.

Field sources:

  evaluator_installed        _cinderx.is_frame_evaluator_installed()
  compile_requests           observe stats: scheduling events + dropped
  compile_success            trigger stats: compiled_function_creations
  compile_rejected           observe stats: events with a refusal result
  unknown_rejects            observe stats: refusal reasons outside the
                             registered set
  machine_code_installed     len(cinderjit.get_compiled_functions())
  machine_code_entries       trigger stats (incremented by the 3.11 entry
                             glue once machine-code execution ships; zero on
                             capability-gated builds by construction)
  executable_alloc_calls     trigger stats (supplementary, not in the minimal
  executable_alloc_bytes     schema but required by the MR-02 gate proof)
  compiled_function_creations trigger stats (supplementary, see above)
  forced_deopt_hits          0 until the site-deopt runner lands (MR-07)
  organic_deopt_hits         0 until deopt counters land (MR-07)

Harness-owned fields (filled by runners, None until then):

  target_modules_attempted   the frozen stdlib surface (the committed
                             libtest_target_modules.txt manifest), published
                             by the libtest gate leg and merged by the
                             unified_report_gate aggregator
  worker_crashes             per-run, filled by the drivers
  live_compiled_functions_at_exit
                             sampled at Python atexit, which precedes
                             jit::finalize() in module teardown; for the
                             gated stage zero-at-exit implies zero after
                             finalize, and the post-finalize contract is
                             enforced at the C level

The refusal-reason registry starts with the capability-gate refusal; the
front-end MR extends it from the bytecode support list and the shape-refusal
registry.  Any refusal outside the registry counts into unknown_rejects,
which the blocking conditions treat as red.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any

# Refusal reasons the gates recognize.  Extended by later MRs; anything else
# is an unknown reject and blocks.
KNOWN_REFUSAL_REASONS: frozenset[str] = frozenset({
    "CINDERX311_JIT_EXEC_DISABLED",
})

RUNTIME_FIELDS = (
    "evaluator_installed",
    "compile_requests",
    "compile_success",
    "compile_rejected",
    "unknown_rejects",
    "machine_code_installed",
    "machine_code_entries",
    "executable_alloc_calls",
    "executable_alloc_bytes",
    "compiled_function_creations",
    "forced_deopt_hits",
    "organic_deopt_hits",
)

HARNESS_FIELDS = (
    "target_modules_attempted",
    "worker_crashes",
    "live_compiled_functions_at_exit",
)

ALL_FIELDS = RUNTIME_FIELDS + HARNESS_FIELDS


def _trigger_stats() -> dict[str, int]:
    import _cinderx

    return _cinderx._get_trigger_stats()


def _observe_stats() -> dict[str, Any] | None:
    import _cinderx

    get_stats = getattr(_cinderx, "_get_observe_stats", None)
    if get_stats is None:
        return None
    return get_stats()


def _evaluator_installed() -> bool:
    import _cinderx

    probe = getattr(_cinderx, "is_frame_evaluator_installed", None)
    if probe is None:
        return False
    return bool(probe())


def _compiled_function_count() -> int:
    try:
        from cinderjit import get_compiled_functions
    except ImportError:
        return 0
    return len(get_compiled_functions())


def snapshot() -> dict[str, Any]:
    """Collect the runtime-derived fields from the current process."""
    trigger = _trigger_stats()
    observe = _observe_stats()

    compile_requests = 0
    compile_rejected = 0
    unknown_rejects = 0
    if observe is not None:
        events = observe.get("events", [])
        compile_requests = len(events) + int(observe.get("events_dropped", 0))
        for event in events:
            result = event.get("result")
            if result in (None, "ok", "compiled"):
                continue
            compile_rejected += 1
            if result not in KNOWN_REFUSAL_REASONS:
                unknown_rejects += 1
        # Events the bounded observe buffer dropped carry no reason; what
        # cannot be classified is counted as unknown (and as rejected, the
        # only outcome this build has), so a lossy buffer can never launder
        # refusals past the unknown_rejects == 0 blocking condition.
        dropped = int(observe.get("events_dropped", 0))
        compile_rejected += dropped
        unknown_rejects += dropped

    report: dict[str, Any] = {
        "evaluator_installed": _evaluator_installed(),
        "compile_requests": compile_requests,
        "compile_success": int(trigger["compiled_function_creations"]),
        "compile_rejected": compile_rejected,
        "unknown_rejects": unknown_rejects,
        "machine_code_installed": _compiled_function_count(),
        "machine_code_entries": int(trigger["machine_code_entries"]),
        "executable_alloc_calls": int(trigger["executable_alloc_calls"]),
        "executable_alloc_bytes": int(trigger["executable_alloc_bytes"]),
        "compiled_function_creations": int(
            trigger["compiled_function_creations"]
        ),
        "forced_deopt_hits": 0,
        "organic_deopt_hits": 0,
    }
    for field in HARNESS_FIELDS:
        report[field] = None
    return report


def validate_schema(report: dict[str, Any], strict: bool = False) -> list[str]:
    """Structural check used by the self-tests: every field present, no
    extras, runtime fields typed.  A child snapshot leaves harness fields
    as None for its own leg to fill; the aggregated unified report passes
    strict=True, where every harness field must be a non-negative int --
    a null field can no longer impersonate a filled one."""
    errors = []
    for field in ALL_FIELDS:
        if field not in report:
            errors.append(f"missing field {field}")
    for field in report:
        if field not in ALL_FIELDS:
            errors.append(f"unknown field {field}")
    if not isinstance(report.get("evaluator_installed"), bool):
        errors.append("evaluator_installed must be a bool")
    for field in RUNTIME_FIELDS[1:]:
        value = report.get(field)
        if not isinstance(value, int) or isinstance(value, bool):
            errors.append(f"{field} must be an int, got {value!r}")
    if strict:
        for field in HARNESS_FIELDS:
            value = report.get(field)
            if not (isinstance(value, int) and not isinstance(value, bool)
                    and value >= 0):
                errors.append(
                    f"{field} must be a non-negative int in the unified "
                    f"report, got {value!r}"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=str, default=None)
    args = parser.parse_args(argv)

    report = snapshot()
    errors = validate_schema(report)
    if errors:
        for error in errors:
            print(f"trigger-report: {error}", file=sys.stderr)
        return 1
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf8") as fp:
            fp.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
