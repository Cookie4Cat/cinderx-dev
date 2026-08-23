"""A2 threshold=1 one-time runtime-footprint plateau probe."""

from __future__ import annotations

import argparse
import gc
import json
from pathlib import Path
import sys


class Box:
    value = 1


def footprint_target(obj):
    return obj.value + 1


def census(_cinderx) -> dict:
    gc.collect()
    stats = _cinderx._get_trigger_stats()
    return {
        "gc_objects": len(gc.get_objects()),
        "resident_code_buffers": int(stats["resident_code_buffers"]),
        "compiled_function_creations": int(stats["compiled_function_creations"]),
        "machine_code_entries": int(stats["machine_code_entries"]),
    }


def run() -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    obj = Box()
    baseline = census(_cinderx)

    for _ in range(20):
        assert footprint_target(obj) == 2
        if cinderjit.is_jit_compiled(footprint_target):
            break
    if not cinderjit.is_jit_compiled(footprint_target):
        raise AssertionError("threshold=1 did not publish footprint_target")
    after_first = census(_cinderx)

    snapshots = {}

    def tracer(frame, event, arg):
        return tracer

    for index in range(1, 101):
        Box.value = index % 2 + 1
        expected = Box.value + 1
        assert footprint_target(obj) == expected
        sys.settrace(tracer)
        assert footprint_target(obj) == expected
        sys.settrace(None)
        assert footprint_target(obj) == expected
        if index in (10, 100):
            snapshots[index] = census(_cinderx)
    after_gc = census(_cinderx)

    first_growth = after_first["gc_objects"] - baseline["gc_objects"]
    steady_growth = snapshots[100]["gc_objects"] - snapshots[10]["gc_objects"]
    resident_growth = (
        snapshots[100]["resident_code_buffers"]
        - after_first["resident_code_buffers"]
    )
    plateau = steady_growth <= 5 and resident_growth <= 1
    return {
        "result": "PASS" if plateau else "REVIEW_REQUIRED",
        "baseline": baseline,
        "after_first_jit": after_first,
        "after_10": snapshots[10],
        "after_100": snapshots[100],
        "after_gc": after_gc,
        "delta": {
            "first_publication_gc_objects": first_growth,
            "steady_10_to_100_gc_objects": steady_growth,
            "steady_resident_buffers": resident_growth,
        },
        "plateau": plateau,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run()
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"], "plateau": report["plateau"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
