"""A2-P threshold-driven worker evidence hook and journal classifier."""

from __future__ import annotations

import argparse
import atexit
from collections import Counter
import json
import os
from pathlib import Path
import sys

from ci_pipeline.jit311.report import KNOWN_REFUSAL_REASONS


def worker_target_module() -> str | None:
    for index, arg in enumerate(sys.argv):
        encoded = None
        if arg == "--worker-args" and index + 1 < len(sys.argv):
            encoded = sys.argv[index + 1]
        elif arg.startswith("--worker-args="):
            encoded = arg.partition("=")[2]
        if encoded is None:
            continue
        try:
            _namespace, test_name = json.loads(encoded)
        except Exception:
            return None
        test_name = str(test_name)
        return test_name if test_name.startswith("test.") else "test." + test_name
    return None


def install_hook() -> None:
    journal = Path(os.environ["A2_PENETRATION_JOURNAL"])
    journal.mkdir(parents=True, exist_ok=True)

    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    cinderjit._jit311_reset_entry_ledger()

    def emit() -> None:
        try:
            trigger = _cinderx._get_trigger_stats()
            observe = _cinderx._get_observe_stats()
            ledger = cinderjit._jit311_entry_ledger()
            rows = [
                {
                    **row,
                    "filename": os.path.realpath(row["filename"]),
                }
                for row in ledger["entries"]
            ]
            payload = {
                "pid": os.getpid(),
                "target_module": worker_target_module(),
                "trigger": trigger,
                "observe": observe,
                "entry_ledger": rows,
                "entry_ledger_dropped": ledger["dropped"],
            }
        except BaseException as exc:
            payload = {
                "pid": os.getpid(),
                "target_module": worker_target_module(),
                "summary_error": f"{type(exc).__name__}: {exc}",
            }
        (journal / f"{os.getpid()}.json").write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    atexit.register(emit)


def _targets(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def classify(journal: Path, target_path: Path, result_path: Path) -> dict:
    target_modules = _targets(target_path)
    test_result = json.loads(result_path.read_text())
    rows: dict[str, dict] = {}
    errors: list[str] = []
    totals: Counter[str] = Counter()
    unknown_refusals: list[dict] = []
    for path in sorted(journal.glob("*.json")):
        payload = json.loads(path.read_text())
        target = payload.get("target_module")
        if payload.get("summary_error"):
            errors.append(f"{path.name}: {payload['summary_error']}")
        if not target:
            continue
        short = target.removeprefix("test.")
        # The target module is gone in this parent process. Resolve its file
        # from the exact code rows: rows owned by Lib/test/<target>.py are the
        # only acceptable own-code evidence.
        suffix = f"/test/{short}.py"
        own = [
            row
            for row in payload.get("entry_ledger", ())
            if os.path.realpath(str(row.get("filename", ""))).endswith(suffix)
            and int(row.get("entries", 0)) > 0
        ]
        trigger = payload.get("trigger", {})
        observe = payload.get("observe", {})
        own_scheduler = [
            event
            for event in observe.get("events", [])
            if os.path.realpath(str(event.get("filename", ""))).endswith(suffix)
        ]
        for event in observe.get("events", []):
            result = event.get("result")
            if result not in KNOWN_REFUSAL_REASONS and result not in {
                "installed",
                "ok",
                "compiled",
                "deferred",
            }:
                unknown_refusals.append(
                    {
                        "target_module": short,
                        "filename": event.get("filename"),
                        "qualname": event.get("qualname"),
                        "result": result,
                    }
                )
        dropped = int(payload.get("entry_ledger_dropped", 0))
        status = "OWN_CODE_JIT" if own else "A2_COVERAGE_GAP"
        rows[short] = {
            "status": status,
            "worker_machine_entries": int(trigger.get("machine_code_entries", 0)),
            "own_code_entries": sum(int(row["entries"]) for row in own),
            "own_code_rows": own,
            "compiled_function_creations": int(
                trigger.get("compiled_function_creations", 0)
            ),
            "organic_deopts": int(trigger.get("organic_deopt_hits", 0)),
            "forced_deopts": int(trigger.get("forced_deopt_hits", 0)),
            "scheduler_events": observe.get("events", []),
            "own_scheduler_events": own_scheduler,
            "discovered_functions": len(
                {event.get("qualname") for event in own_scheduler}
            ),
            "observed_call_count": sum(
                int(event.get("count", 0)) for event in own_scheduler
            ),
            "compile_results": dict(
                Counter(str(event.get("result")) for event in own_scheduler)
            ),
            "events_dropped": int(observe.get("events_dropped", 0)),
            "entry_ledger_dropped": dropped,
        }
        totals["machine_entries"] += rows[short]["worker_machine_entries"]
        totals["compiled_function_creations"] += rows[short][
            "compiled_function_creations"
        ]
        totals["organic_deopts"] += rows[short]["organic_deopts"]
        totals["forced_deopts"] += rows[short]["forced_deopts"]
        totals["ledger_dropped"] += dropped

    missing = sorted(set(target_modules) - set(rows))
    if missing:
        errors.append(f"missing worker summaries: {missing}")
    for target in target_modules:
        if target not in rows:
            rows[target] = {"status": "A2_COVERAGE_GAP", "missing": True}
    counts = Counter(row["status"] for row in rows.values())
    result = {
        "result": "PASS"
        if len(target_modules) == 72
        and counts["OWN_CODE_JIT"] == 72
        and not errors
        and totals["ledger_dropped"] == 0
        and not unknown_refusals
        else "FAIL",
        "target_modules": len(target_modules),
        "counts": dict(counts),
        "totals": dict(totals),
        "modules": rows,
        "errors": errors,
        "unknown_refusals": unknown_refusals,
        "test_modules": test_result.get("modules", {}),
    }
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--test-result", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = classify(args.journal, args.targets, args.test_result)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"], **report["counts"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if os.environ.get("A2_PENETRATION_JOURNAL"):
    install_hook()


if __name__ == "__main__":
    raise SystemExit(main())
