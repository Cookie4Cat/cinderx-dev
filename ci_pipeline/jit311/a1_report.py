"""A1 Compile-All classification, exact deviation matching, and reporting."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import tomllib

from ci_pipeline.libtest_diff_311 import diff_results_symmetric, load


def _lines(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def load_capabilities(path: Path) -> tuple[set[str], set[str]]:
    with path.open("rb") as stream:
        document = tomllib.load(stream)
    return (
        set(document["expected_refusal"]["reasons"]),
        set(document["runtime_fallback"]["reasons"]),
    )


def read_journal(directory: Path) -> list[dict]:
    events: list[dict] = []
    for path in sorted(directory.glob("*.jsonl")):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid journal {path}:{number}: {exc}") from exc
    return events


def classify_compile_all(
    journal: Path, targets_path: Path, capabilities_path: Path
) -> dict:
    targets = _lines(targets_path)
    if len(targets) != 72 or len(targets) != len(set(targets)):
        raise ValueError(f"A1 target manifest must contain 72 unique modules, got {len(targets)}")
    expected_reasons, runtime_reasons = load_capabilities(capabilities_path)
    events = read_journal(journal)

    functions: dict[tuple[str, int, str], dict] = {}
    entered_functions: set[tuple[str, int, str]] = set()
    scans: dict[str, list[dict]] = defaultdict(list)
    entry_deltas: Counter[str] = Counter()
    for event in events:
        kind = event.get("type")
        if kind == "compile":
            key = (
                str(event.get("filename")),
                int(event.get("firstlineno", -1)),
                str(event.get("qualname")),
            )
            prior = functions.get(key)
            # Prefer successful compilation evidence when the same function is
            # rediscovered through an alias; otherwise keep the first exact
            # typed verdict.
            if prior is None or (
                prior.get("status") != "compiled" and event.get("status") == "compiled"
            ):
                functions[key] = event
        elif kind == "module-scan":
            scans[str(event.get("module"))].append(event)
        elif kind in ("test-call", "doctest-call"):
            target = event.get("target_module")
            if target:
                entry_deltas[str(target)] += max(0, int(event.get("machine_entries_delta", 0)))
            if (
                kind == "test-call"
                and event.get("compile_status") == "compiled"
                and int(event.get("machine_entries_delta", 0)) > 0
                and event.get("filename") is not None
            ):
                entered_functions.add(
                    (
                        str(event["filename"]),
                        int(event.get("firstlineno", -1)),
                        str(event.get("method_qualname")),
                    )
                )

    reason_counts: Counter[str] = Counter()
    counters = Counter()
    unexpected: list[dict] = []
    unknown: list[dict] = []
    for event in functions.values():
        counters["discovered"] += 1
        counters["attempted"] += 1
        status = event.get("status")
        reason = event.get("reason")
        if status == "compiled":
            counters["compiled"] += 1
        elif status == "runtime-fallback" and reason in runtime_reasons:
            counters["runtime_fallback"] += 1
            reason_counts[str(reason)] += 1
        elif reason in expected_reasons:
            counters["expected_refusal"] += 1
            reason_counts[str(reason)] += 1
        elif reason is None or status in ("unknown-refusal", "hook-error"):
            counters["unknown_refusal"] += 1
            unknown.append(event)
        else:
            counters["unexpected_refusal"] += 1
            reason_counts[str(reason)] += 1
            unexpected.append(event)

    module_results: dict[str, dict] = {}
    for short_name in targets:
        full_name = short_name if short_name.startswith("test.") else "test." + short_name
        relevant = scans.get(full_name, []) + scans.get(short_name, [])
        discovered = sum(int(scan.get("discovered", 0)) for scan in relevant)
        statuses = Counter()
        reasons = Counter()
        for scan in relevant:
            statuses.update(scan.get("statuses", {}))
            reasons.update(scan.get("reasons", {}))

        if entry_deltas[full_name] > 0 or entry_deltas[short_name] > 0:
            classification = "JIT_EXECUTED"
        elif any(reason in runtime_reasons for reason in reasons):
            classification = "RUNTIME_FALLBACK"
        elif discovered == 0:
            classification = "EXPECTED_SAFE_REFUSAL"
            reasons["REFUSE_SHAPE_NON_FUNCTION_SCOPE"] += 1
        elif (
            statuses["compiled"] == 0
            and sum(reasons.values()) >= discovered
            and all(reason in expected_reasons for reason in reasons)
        ):
            classification = "EXPECTED_SAFE_REFUSAL"
        else:
            classification = "UNCOVERED"
        module_results[short_name] = {
            "classification": classification,
            "discovered": discovered,
            "machine_entries": entry_deltas[full_name] + entry_deltas[short_name],
            "statuses": dict(statuses),
            "reasons": dict(reasons),
        }

    module_counts = Counter(item["classification"] for item in module_results.values())
    counters["entered"] = len(entered_functions & set(functions))
    for name in (
        "discovered",
        "attempted",
        "compiled",
        "entered",
        "expected_refusal",
        "runtime_fallback",
        "unexpected_refusal",
        "unknown_refusal",
    ):
        counters[name] += 0
    for name in (
        "JIT_EXECUTED",
        "EXPECTED_SAFE_REFUSAL",
        "RUNTIME_FALLBACK",
        "UNCOVERED",
    ):
        module_counts[name] += 0
    result = {
        "functions": dict(counters),
        "refusals_by_reason": dict(sorted(reason_counts.items())),
        "modules": module_results,
        "module_counts": dict(module_counts),
        "unexpected_refusals": unexpected,
        "unknown_refusals": unknown,
        "journal_events": len(events),
    }
    result["result"] = (
        "PASS"
        if module_counts["UNCOVERED"] == 0
        and counters["unknown_refusal"] == 0
        and counters["unexpected_refusal"] == 0
        else "FAIL"
    )
    return result


def load_deviations(path: Path, lane: str = "C") -> tuple[dict, list[dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    entries = [item for item in document.get("deviations", []) if item.get("lane") == lane]
    allowed: dict[str, dict[str, str]] = {}
    for item in entries:
        testcase = item["testcase"]
        if testcase in allowed:
            raise ValueError(f"duplicate deviation testcase: {testcase}")
        allowed[testcase] = {
            "stock": item["stock_observable"],
            "execute": item["jit_observable"],
        }
    return allowed, entries


def compare_with_deviations(
    stock_path: Path, execute_path: Path, deviations_path: Path
) -> dict:
    allowed, entries = load_deviations(deviations_path)
    raw = diff_results_symmetric(load(str(stock_path)), load(str(execute_path)), allowed)

    # Module failure is a summary, never an approval unit.  Suppress only the
    # test_dis summary when every concrete test_dis difference is an exact,
    # approved case.  Any new case difference keeps the summary unexpected.
    dis_concrete = {
        key for key in raw["differences"] if key.startswith("test.test_dis.")
    }
    if (
        raw["differences"].get("<module> test_dis") == {"stock": "pass", "execute": "fail"}
        and dis_concrete
        and dis_concrete <= set(allowed)
    ):
        raw["unexpected"].pop("<module> test_dis", None)
        raw["derived_module_summaries"] = {
            "<module> test_dis": raw["differences"].pop("<module> test_dis")
        }
    else:
        raw["derived_module_summaries"] = {}

    raw["approved_deviations"] = entries
    raw["result"] = (
        "FAIL"
        if raw["unexpected"]
        else "REVIEW_REQUIRED"
        if raw["stale_baseline"]
        else "PASS_WITH_APPROVED_DEVIATIONS"
        if raw["differences"]
        else "PASS"
    )
    return raw


def write_markdown(report: dict, path: Path) -> None:
    functions = report["functions"]
    modules = report["module_counts"]
    lines = [
        "# A1 Compile-All Classification",
        "",
        f"Result: **{report['result']}**",
        "",
        "| Evidence | Count |",
        "|---|---:|",
        f"| Functions discovered | {functions.get('discovered', 0)} |",
        f"| Compile attempted | {functions.get('attempted', 0)} |",
        f"| Compiled | {functions.get('compiled', 0)} |",
        f"| Functions entered | {functions.get('entered', 0)} |",
        f"| Expected refusal | {functions.get('expected_refusal', 0)} |",
        f"| Runtime fallback | {functions.get('runtime_fallback', 0)} |",
        f"| Unexpected refusal | {functions.get('unexpected_refusal', 0)} |",
        f"| Unknown refusal | {functions.get('unknown_refusal', 0)} |",
        "",
        "| Module classification | Count |",
        "|---|---:|",
    ]
    for name in ("JIT_EXECUTED", "EXPECTED_SAFE_REFUSAL", "RUNTIME_FALLBACK", "UNCOVERED"):
        lines.append(f"| {name} | {modules.get(name, 0)} |")
    lines.extend(["", "## Refusals by reason", ""])
    for reason, count in report["refusals_by_reason"].items():
        lines.append(f"- `{reason}`: {count}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    classify = subparsers.add_parser("classify")
    classify.add_argument("--journal", type=Path, required=True)
    classify.add_argument("--targets", type=Path, required=True)
    classify.add_argument("--capabilities", type=Path, required=True)
    classify.add_argument("--out", type=Path, required=True)
    classify.add_argument("--markdown", type=Path)

    compare = subparsers.add_parser("compare")
    compare.add_argument("--stock", type=Path, required=True)
    compare.add_argument("--execute", type=Path, required=True)
    compare.add_argument("--deviations", type=Path, required=True)
    compare.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.command == "classify":
        report = classify_compile_all(args.journal, args.targets, args.capabilities)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        if args.markdown:
            write_markdown(report, args.markdown)
    else:
        report = compare_with_deviations(args.stock, args.execute, args.deviations)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"]}, sort_keys=True))
    return 0 if report["result"] in ("PASS", "PASS_WITH_APPROVED_DEVIATIONS") else 1


if __name__ == "__main__":
    raise SystemExit(main())
