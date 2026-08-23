"""A2 penetration differential, transition proof judge, and Markdown report."""

from __future__ import annotations

import json
from pathlib import Path
import tomllib

from ci_pipeline.libtest_diff_311 import diff_results_symmetric, load


PASS_STATES = {"PASS", "PASS_WITH_APPROVED_DEVIATIONS"}


def compare_penetration(
    stock_path: Path,
    aggressive_path: Path,
    deviation_path: Path,
) -> dict:
    stock = load(str(stock_path))
    aggressive = load(str(aggressive_path))
    document = json.loads(deviation_path.read_text())
    deviations = document.get("deviations", [])
    allowed = {
        item["testcase"]: {
            "stock": item["stock"],
            "execute": item["aggressive"],
        }
        for item in deviations
    }
    report = diff_results_symmetric(stock, aggressive, allowed)
    fingerprints = {}
    for item in deviations:
        testcase = item["testcase"]
        if report["differences"].get(testcase) != allowed[testcase]:
            continue
        diagnostic = aggressive.get("diagnostics", {}).get(testcase, "")
        missing = [part for part in item.get("fingerprint", []) if part not in diagnostic]
        fingerprints[testcase] = {"matched": not missing, "missing": missing}
        if missing:
            report["unexpected"][testcase] = {
                **allowed[testcase],
                "diagnostic_fingerprint_missing": missing,
            }
    concrete = {
        key for key in report["differences"] if key.startswith("test.test_dis.")
    }
    if (
        report["differences"].get("<module> test_dis")
        == {"stock": "pass", "execute": "fail"}
        and concrete
        and concrete <= set(allowed)
    ):
        report["unexpected"].pop("<module> test_dis", None)
        report["derived_module_summary"] = report["differences"].pop(
            "<module> test_dis"
        )
    derived_modules = {}
    for key in list(report["differences"]):
        if not key.startswith("<module> "):
            continue
        module = key.removeprefix("<module> ")
        if any(
            case.startswith(f"test.{module}.")
            for case in report["differences"]
            if not case.startswith("<module> ")
        ):
            derived_modules[key] = report["differences"].pop(key)
            report["unexpected"].pop(key, None)
    report["derived_module_summaries"] = derived_modules
    report["fingerprints"] = fingerprints
    report["approved_deviations"] = deviations
    report["result"] = (
        "FAIL"
        if report["unexpected"]
        else "REVIEW_REQUIRED"
        if report["stale_baseline"]
        else "PASS_WITH_APPROVED_DEVIATIONS"
        if report["differences"]
        else "PASS"
    )
    return report


def judge_transitions(
    stock_path: Path,
    jit_path: Path,
    manifest_path: Path,
) -> dict:
    stock = json.loads(stock_path.read_text())
    jit = json.loads(jit_path.read_text())
    with manifest_path.open("rb") as stream:
        manifest = tomllib.load(stream)
    specs = {item["id"]: item for item in manifest["transition"]}
    stock_rows = {item["id"]: item for item in stock.get("transitions", [])}
    jit_rows = {item["id"]: item for item in jit.get("transitions", [])}
    rows = []
    for ident in sorted(specs):
        spec = specs[ident]
        stock_row = stock_rows.get(ident)
        jit_row = jit_rows.get(ident)
        errors = []
        if stock_row is None or jit_row is None:
            errors.append("missing stock or JIT row")
        else:
            if stock_row.get("semantic") != jit_row.get("semantic"):
                errors.append("semantic result differs from Stock")
            pre = jit_row.get("pre", {})
            if not pre.get("compiled") or not pre.get("machine_entry_proven"):
                errors.append("pre-state lacks compiled machine-entry proof")
            transition = jit_row.get("transition", {})
            if transition.get("transition_ledger_dropped", 0) != 0:
                errors.append("transition ledger dropped evidence")
            reasons = {
                row.get("deopt_reason")
                for row in transition.get("transition_rows", [])
            }
            required = set(spec.get("requires_transition_reason", []))
            if required and not (required & reasons):
                errors.append(
                    f"required deopt reason absent: expected one of {sorted(required)}, got {sorted(str(r) for r in reasons)}"
                )
            recovery = jit_row.get("recovery", {})
            if spec["recovery"] not in (
                "interpreter-resume",
                "interpreter-after-backoff",
            ) and not (
                recovery.get("reentered")
                or recovery.get("machine_entry_proven")
                or recovery.get("compiled")
            ):
                errors.append("recovery lacks JIT re-entry/recompile proof")
            if jit_row.get("result") != "PASS":
                errors.append("probe self-check failed")
        rows.append(
            {
                "id": ident,
                "name": spec["name"],
                "pre_jit": bool(jit_row and jit_row.get("pre", {}).get("machine_entry_proven")),
                "trigger": spec["probe"],
                "transition_proof": jit_row.get("transition", {}) if jit_row else None,
                "stock_match": not errors or "semantic result differs from Stock" not in errors,
                "recovery": jit_row.get("recovery", {}) if jit_row else None,
                "errors": errors,
                "result": "PASS" if not errors else "FAIL",
            }
        )
    return {
        "result": "PASS"
        if len(rows) == 10 and all(row["result"] == "PASS" for row in rows)
        else "FAIL",
        "transitions": rows,
        "unknown_transitions": sorted((set(stock_rows) | set(jit_rows)) - set(specs)),
    }


def render_markdown(final: dict, path: Path) -> None:
    penetration = final.get("penetration", {})
    aggressive = penetration.get("aggressive_coverage", {})
    transitions = final.get("transitions", {})
    repetition = final.get("repetition", {})
    footprint = final.get("footprint", {})
    lines = [
        "# CPython 3.11 CinderX JIT A2 Execution Report",
        "",
        f"Final: **{final['final']}**",
        "",
        "## A2-P Aggressive penetration",
        "",
        f"- Target modules: {aggressive.get('target_modules', 0)}",
        f"- Worker/own-code JIT: {aggressive.get('counts', {}).get('OWN_CODE_JIT', 0)}/72",
        f"- Coverage gaps: {aggressive.get('counts', {}).get('A2_COVERAGE_GAP', 0)}",
        f"- Unknown refusals: {len(aggressive.get('unknown_refusals', []))}",
        f"- Differential: {penetration.get('differential', {}).get('result', 'NOT_RUN')}",
        f"- Unexpected differences: {len(penetration.get('differential', {}).get('unexpected', {}))}",
        "",
        "## A2-T Transition matrix",
        "",
        "| Transition | Pre JIT | Trigger | Stock match | Recovery | Result |",
        "|---|---|---|---|---|---|",
    ]
    for row in transitions.get("transitions", []):
        lines.append(
            f"| {row['id']} {row['name']} | {row['pre_jit']} | {row['trigger']} | {row['stock_match']} | {json.dumps(row['recovery'], sort_keys=True)} | {row['result']} |"
        )
    lines.extend(
        [
            "",
            "Aggressive tracing/C-trace regression: "
            f"**{transitions.get('aggressive_tracing_regression', {}).get('result', 'NOT_RUN')}**",
        ]
    )
    lines.extend(
        [
            "",
            "## A2-R Repetition",
            "",
            "| Transition | Cycles | Semantic failures | State failures |",
            "|---|---:|---:|---:|",
        ]
    )
    for row in repetition.get("transitions", []):
        lines.append(
            f"| {row['transition']} | {row['cycles']} | {row['semantic_failures']} | {row['state_failures']} |"
        )
    lines.extend(
        [
            "",
            "## Footprint plateau",
            "",
            f"- Result: {footprint.get('result', 'NOT_RUN')}",
            f"- Plateau: {footprint.get('plateau')}",
            f"- Delta: `{json.dumps(footprint.get('delta', {}), sort_keys=True)}`",
            "",
            "## Blockers",
            "",
        ]
    )
    if final.get("blockers"):
        lines.extend(
            f"- **{item['id']}** ({item['severity']}): {item['summary']}"
            for item in final["blockers"]
        )
    else:
        lines.append("- None")
    path.write_text("\n".join(lines) + "\n")
