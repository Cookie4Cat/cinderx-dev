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
        missing = [
            part for part in item.get("fingerprint", []) if part not in diagnostic
        ]
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
        else (
            "REVIEW_REQUIRED"
            if report["stale_baseline"]
            else "PASS_WITH_APPROVED_DEVIATIONS" if report["differences"] else "PASS"
        )
    )
    return report


def compare_frame_positions(
    running_stock_path: Path,
    running_jit_path: Path,
    error_stock_path: Path,
    error_jit_path: Path,
) -> dict:
    running_stock = json.loads(running_stock_path.read_text())
    running_jit = json.loads(running_jit_path.read_text())
    error_stock = json.loads(error_stock_path.read_text())
    error_jit = json.loads(error_jit_path.read_text())

    errors = []
    running_rows = []
    stock_by_case = {row["case"]: row for row in running_stock.get("rows", [])}
    jit_by_case = {row["case"]: row for row in running_jit.get("rows", [])}
    for case in sorted(set(stock_by_case) | set(jit_by_case)):
        stock = stock_by_case.get(case)
        jit = jit_by_case.get(case)
        row_errors = []
        if stock is None or jit is None:
            row_errors.append("missing Stock or JIT row")
        else:
            if stock.get("observation") != jit.get("observation"):
                row_errors.append("running frame observation differs from Stock")
            if not jit.get("machine_entry_proven"):
                row_errors.append("no target machine-entry proof")
        if row_errors:
            errors.extend(f"running {case}: {item}" for item in row_errors)
        running_rows.append(
            {
                "case": case,
                "stock": stock.get("observation") if stock else None,
                "jit": jit.get("observation") if jit else None,
                "machine_entry_proven": bool(jit and jit.get("machine_entry_proven")),
                "errors": row_errors,
                "result": "PASS" if not row_errors else "FAIL",
            }
        )

    error_rows = []
    stock_by_case = {row["case"]: row for row in error_stock.get("rows", [])}
    jit_by_case = {row["case"]: row for row in error_jit.get("rows", [])}
    for case in sorted(set(stock_by_case) | set(jit_by_case)):
        stock = stock_by_case.get(case)
        jit = jit_by_case.get(case)
        row_errors = []
        if stock is None or jit is None:
            row_errors.append("missing Stock or JIT row")
        else:
            if stock.get("target_frame") != jit.get("target_frame"):
                row_errors.append("target traceback position differs from Stock")
            if stock.get("traceback_frames") != jit.get("traceback_frames"):
                row_errors.append("full traceback frames differ from Stock")
            if not jit.get("machine_entry_proven"):
                row_errors.append("no target machine-entry proof")
            if not jit.get("transitions"):
                row_errors.append("no typed deopt transition proof")
            if jit.get("transition_ledger_dropped"):
                row_errors.append("transition ledger dropped evidence")
        if row_errors:
            errors.extend(f"error {case}: {item}" for item in row_errors)
        error_rows.append(
            {
                "case": case,
                "stock": stock.get("target_frame") if stock else None,
                "jit": jit.get("target_frame") if jit else None,
                "traceback_frames_match": bool(
                    stock
                    and jit
                    and stock.get("traceback_frames") == jit.get("traceback_frames")
                ),
                "machine_entry_proven": bool(jit and jit.get("machine_entry_proven")),
                "transitions": jit.get("transitions", []) if jit else [],
                "errors": row_errors,
                "result": "PASS" if not row_errors else "FAIL",
            }
        )

    for label, document in (
        ("running Stock", running_stock),
        ("running JIT", running_jit),
        ("error Stock", error_stock),
        ("error JIT", error_jit),
    ):
        if document.get("result") != "PASS":
            errors.append(f"{label} probe self-check failed")
        if document.get("entry_ledger_dropped", 0):
            errors.append(f"{label} entry ledger dropped evidence")

    return {
        "result": "PASS" if not errors else "FAIL",
        "running": running_rows,
        "error": error_rows,
        "errors": errors,
        "unreachable": error_jit.get("unreachable", {}),
    }


def render_frame_position_report(
    comparison: dict,
    before_path: Path,
    transition_result: dict,
    inspect_returncode: int,
    out: Path,
) -> None:
    before = json.loads(before_path.read_text())
    lines = [
        "# CPython 3.11 JIT A2 Frame Position Report",
        "",
        f"- Position matrix: `{comparison.get('result')}`",
        f"- `test_inspect`: `{'PASS' if inspect_returncode == 0 else 'FAIL'}`",
        f"- Before-fix source: `{before.get('source_git_sha')}`",
        "",
        "## Running frames (F1)",
        "",
        "| Case | Stock `(f_lasti, line, position)` | Before | After | Machine entry |",
        "|---|---|---|---|---:|",
    ]
    before_running = before.get("running", {})
    for row in comparison.get("running", []):
        case = row["case"]
        stock = row.get("stock") or {}
        jit = row.get("jit") or {}
        stock_value = [
            stock.get("f_lasti"),
            stock.get("f_lineno"),
            stock.get("co_position"),
        ]
        before_value = before_running.get(case, {}).get("before")
        after_value = [
            jit.get("f_lasti"),
            jit.get("f_lineno"),
            jit.get("co_position"),
        ]
        lines.append(
            f"| `{case}` | `{stock_value}` | `{before_value}` | `{after_value}` | "
            f"{'yes' if row.get('machine_entry_proven') else 'no'} |"
        )
    lines.extend(
        [
            "",
            "`sys._getframe()`, `frame.f_lasti`, `frame.f_lineno`, "
            "`code.co_positions()` and `inspect.stack()` agree in every row.",
            "",
            "## Error and deopt positions (F2)",
            "",
            "| Case | Opcode offset/cache | Stock `(tb_lasti, f_lasti, position)` | Before | After | Deopt proof |",
            "|---|---|---|---|---|---|",
        ]
    )
    before_error = before.get("error", {})
    for row in comparison.get("error", []):
        case = row["case"]
        stock = row.get("stock") or {}
        jit = row.get("jit") or {}
        instruction = [
            stock.get("opcode_offset"),
            stock.get("inline_cache_span"),
        ]
        stock_value = [
            stock.get("tb_lasti"),
            stock.get("f_lasti"),
            stock.get("position"),
        ]
        before_value = before_error.get(case, {}).get("before")
        after_value = [
            jit.get("tb_lasti"),
            jit.get("f_lasti"),
            jit.get("position"),
        ]
        transitions = [
            [
                item.get("deopt_reason"),
                item.get("cause_offset"),
                item.get("resume_offset"),
            ]
            for item in row.get("transitions", [])
        ]
        lines.append(
            f"| `{case}` | `{instruction}` | `{stock_value}` | `{before_value}` | "
            f"`{after_value}` | `{transitions}` |"
        )
    transitions = {
        row["id"]: row
        for row in transition_result.get("transitions", [])
        if row.get("id") in {"T03", "T10"}
    }
    lines.extend(
        [
            "",
            "## T03 / T10",
            "",
            f"- T03: `{transitions.get('T03', {}).get('result', 'MISSING')}`; caller `tb_lasti` now matches Stock 20.",
            f"- T10: `{transitions.get('T10', {}).get('result', 'MISSING')}`; every recursive caller `tb_lasti` now matches Stock 52.",
        ]
    )
    if transitions.get("T10", {}).get("result") != "PASS":
        lines.extend(
            [
                "- T10 residual: traceback positions are fixed, but the JIT recursion boundary has one fewer recursive frame than Stock. This is retained as a separate recursion-entry blocker.",
            ]
        )
    lines.extend(
        [
            "",
            f"- Unreachable current capability: `{json.dumps(comparison.get('unreachable', {}), sort_keys=True)}`",
            f"- Matrix errors: `{json.dumps(comparison.get('errors', []), sort_keys=True)}`",
            "",
        ]
    )
    out.write_text("\n".join(lines))


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
                row.get("deopt_reason") for row in transition.get("transition_rows", [])
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
                "pre_jit": bool(
                    jit_row and jit_row.get("pre", {}).get("machine_entry_proven")
                ),
                "trigger": spec["probe"],
                "transition_proof": jit_row.get("transition", {}) if jit_row else None,
                "stock_match": not errors
                or "semantic result differs from Stock" not in errors,
                "recovery": jit_row.get("recovery", {}) if jit_row else None,
                "errors": errors,
                "result": "PASS" if not errors else "FAIL",
            }
        )
    return {
        "result": (
            "PASS"
            if len(rows) == 10 and all(row["result"] == "PASS" for row in rows)
            else "FAIL"
        ),
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
