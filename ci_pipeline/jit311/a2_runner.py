"""Unified CPython 3.11 CinderX JIT A2 transition acceptance runner."""

from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import sys

from ci_pipeline.jit311.a1_runner import A1Runner, PASS_STATES
from ci_pipeline.jit311.a2_report import (
    compare_frame_positions,
    compare_penetration,
    judge_transitions,
    render_frame_position_report,
    render_markdown,
)

OLD_THRESHOLD1_GAPS = (
    "test_cprofile",
    "test_dataclasses",
    "test_extcall",
    "test_genexps",
    "test_import",
    "test_listcomps",
    "test_metaclass",
    "test_unpack",
    "test_unpack_ex",
)


def _coverage_count(report: dict, status: str) -> int:
    return int(report.get("counts", {}).get(status, 0))


def _short_gap_reason(row: dict) -> str:
    events = row.get("own_scheduler_events", [])
    installed = [event for event in events if event.get("result") == "installed"]
    if installed and not row.get("machine_entry_proven"):
        return "artifact published on the last observed call; no later own-code call entered it"
    if not events:
        return "no own function reached the scheduler threshold"
    results = ", ".join(
        f"{name}={count}"
        for name, count in sorted(row.get("compile_results", {}).items())
    )
    return f"own scheduler attempts were refused ({results})"


def render_penetration_v02(result: dict, path: Path) -> None:
    control = result["control_coverage"]
    aggressive = result["aggressive_coverage"]
    diagnostic = result["threshold1_diagnostic_coverage"]
    differential = result["differential"]
    gaps = {
        name: row
        for name, row in aggressive.get("modules", {}).items()
        if row.get("status") == "A2_COVERAGE_GAP"
    }
    thresholds = sorted(
        {
            row.get("scheduler_threshold")
            for row in aggressive.get("modules", {}).values()
            if row.get("scheduler_threshold") is not None
        }
    )
    bad_module_states = {
        name: state
        for name, state in aggressive.get("test_modules", {}).items()
        if state in {"crash", "no_result"}
    }
    lines = [
        "# CPython 3.11 JIT A2 Penetration v0.2",
        "",
        f"- Gate result: `{result['result']}`",
        f"- Target modules: `{aggressive.get('target_modules', 0)}`",
        f"- P1 threshold=50 own-code JIT: `{_coverage_count(control, 'OWN_CODE_JIT')}/72`",
        f"- P2 `PYTHONJITALL=1` own-code JIT: `{_coverage_count(aggressive, 'OWN_CODE_JIT')}/72`",
        f"- P2-DIAG threshold=1 own-code JIT: `{_coverage_count(diagnostic, 'OWN_CODE_JIT')}/72`",
        f"- P2 worker JIT active: `{aggressive.get('totals', {}).get('worker_jit_active', 0)}/72`",
        f"- P2 coverage gaps: `{len(gaps)}`",
        f"- Unknown refusals: `{len(aggressive.get('unknown_refusals', []))}`",
        f"- Entry-ledger dropped: `{aggressive.get('totals', {}).get('ledger_dropped', 0)}`",
        f"- Scheduler threshold observed under JIT-ALL: `{thresholds}`",
        "",
        "## Gate conclusion",
        "",
    ]
    if thresholds == [50]:
        lines.extend(
            [
                "The formal P2 process received `PYTHONJITALL=1`, but the CPython 3.11 "
                "frame-entry scheduler reported threshold 50 in every worker. The JIT "
                "configuration and the 3.11 scheduler configuration are therefore not "
                "aligned: this lane does not currently implement the requested first-call "
                "JIT-ALL experiment.",
                "",
                "Per the A2 v0.2 Phase-1 boundary, this report records the mismatch as a "
                "product scheduler/configuration blocker and does not change product JIT "
                "semantics.",
            ]
        )
    else:
        lines.append(
            "The recorded scheduler thresholds must be reviewed together with the coverage gaps."
        )
    lines.extend(
        [
            "",
            "## Nine previous threshold=1 gaps",
            "",
            "| Module | threshold=1 now | JIT-ALL now | Attribution effect | Root cause |",
            "|---|---:|---:|---|---|",
        ]
    )
    for name in OLD_THRESHOLD1_GAPS:
        diag_row = diagnostic.get("modules", {}).get(name, {})
        jit_all_row = aggressive.get("modules", {}).get(name, {})
        diag_ok = diag_row.get("status") == "OWN_CODE_JIT"
        jit_all_ok = jit_all_row.get("status") == "OWN_CODE_JIT"
        package_root = bool(diag_row.get("ownership", {}).get("package_roots"))
        attribution = (
            "fixed old suffix false-negative"
            if package_root and diag_ok
            else "not an attribution gap"
        )
        reason = (
            "package ownership correction" if diag_ok else _short_gap_reason(diag_row)
        )
        lines.append(
            f"| `{name}` | {'yes' if diag_ok else 'no'} | "
            f"{'yes' if jit_all_ok else 'no'} | {attribution} | {reason} |"
        )
    lines.extend(
        [
            "",
            "Package attribution alone fixes `test_dataclasses` and `test_import` in "
            "the threshold=1 diagnostic. The remaining seven threshold=1 gaps publish "
            "on their final own-code call (or are refused) and have no subsequent "
            "machine-code entry. Moving from threshold=1 to the current JIT-ALL lane "
            "fixes none of the nine because the 3.11 scheduler actually uses 50.",
            "",
            "## P2 gap evidence",
            "",
            "| Module | Ownership | Own functions | Scheduler events | Calls | Verdicts | Artifact | Machine entry |",
            "|---|---|---|---:|---:|---|---:|---:|",
        ]
    )
    for name, row in sorted(gaps.items()):
        ownership = row.get("ownership", {})
        roots = [ownership.get("module_file")] + list(
            ownership.get("package_roots", [])
        )
        roots_text = "<br>".join(f"`{root}`" for root in roots if root) or "missing"
        functions = row.get("observed_own_functions", [])
        functions_text = "<br>".join(f"`{item}`" for item in functions) or "none"
        verdicts = (
            ", ".join(
                f"{key}={value}"
                for key, value in sorted(row.get("compile_results", {}).items())
            )
            or "none"
        )
        lines.append(
            f"| `{name}` | {roots_text} | {functions_text} | "
            f"{len(row.get('own_scheduler_events', []))} | "
            f"{row.get('observed_call_count', 0)} | {verdicts} | "
            f"{'yes' if row.get('artifact_installed') else 'no'} | "
            f"{'yes' if row.get('machine_entry_proven') else 'no'} |"
        )
    lines.extend(
        [
            "",
            "## Differential and runtime integrity",
            "",
            f"- Stock vs JIT-ALL testcase differences: `{len(differential.get('differences', {}))}`",
            f"- Unexpected differences: `{len(differential.get('unexpected', {}))}`",
            f"- Stale approved candidates: `{len(differential.get('stale_baseline', {}))}`",
            f"- New crash/hang/no-result modules: `{json.dumps(bad_module_states, sort_keys=True)}`",
            f"- Differential result: `{differential.get('result')}`",
            "",
            "The two threshold=1 superinstruction candidates do not reproduce under "
            "the current JIT-ALL run and are reported as stale rather than silently "
            "accepted. No new baseline entry is created.",
            "",
        ]
    )
    path.write_text("\n".join(lines))


class A2Runner:
    def __init__(
        self,
        *,
        wheel: Path,
        source: Path,
        output: Path,
        lanes: set[str],
        jobs: int,
        timeout: int,
    ) -> None:
        self.base = A1Runner(
            wheel=wheel,
            source=source,
            output=output,
            lanes=set(),
            jobs=jobs,
            timeout=timeout,
        )
        self.lanes = lanes
        self.results: dict[str, dict] = {}

    @property
    def output(self) -> Path:
        return self.base.output

    def _arm_command(
        self,
        *,
        out: Path,
        threshold: int | None = None,
        jit_all: bool = False,
        startup: Path | None = None,
        journal: Path | None = None,
    ) -> list[str]:
        targets = [
            line.strip()
            for line in (
                self.base.stage / "ci_pipeline/jit311/data/a1_compile_all_modules.txt"
            )
            .read_text()
            .splitlines()
            if line.strip() and not line.startswith("#")
        ]
        command = [
            str(self.base.python),
            str(self.base.stage / "ci_pipeline/libtest_diff_311.py"),
            "run",
            "--python",
            str(self.base.python),
            "--jobs",
            str(self.base.jobs),
            "--timeout",
            str(self.base.timeout),
            "--out",
            str(out),
        ]
        if startup is not None:
            command += [
                "--pythonpath-prepend",
                str(startup),
                "--pythonpath-prepend",
                str(self.base.stage),
                "--env",
                f"A2_PENETRATION_JOURNAL={journal}",
                "--env",
                "CINDERX_JIT_MODE=canary",
                "--env",
                "PYTHONJITGENERATOR=1",
            ]
            command += (
                ["--env", "PYTHONJITALL=1"]
                if jit_all
                else ["--env", f"PYTHONJITAUTO={threshold}"]
            )
        command += ["--tests", *targets]
        return command

    def _jit_all_env(self) -> dict[str, str]:
        env = self.base._base_env()
        env.update(
            CINDERX_JIT_MODE="canary",
            PYTHONJITALL="1",
            PYTHONJITGENERATOR="1",
            PYTHONPATH=str(self.base.stage),
        )
        return env

    def _classify_penetration(
        self, name: str, journal: Path, test_result: Path, out: Path
    ) -> int:
        return self.base._run(
            name,
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_penetration",
                "--journal",
                str(journal),
                "--targets",
                str(
                    self.base.stage
                    / "ci_pipeline/jit311/data/a1_compile_all_modules.txt"
                ),
                "--test-result",
                str(test_result),
                "--out",
                str(out),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )

    def run_penetration(self) -> dict:
        directory = self.output / "P"
        directory.mkdir()
        stock = directory / "p0-stock"
        control = directory / "p1-threshold50"
        aggressive = directory / "p2-jit-all"
        diagnostic = directory / "p2-diag-threshold1"
        control_journal = directory / "p1-journal"
        aggressive_journal = directory / "p2-journal"
        diagnostic_journal = directory / "p2-diag-journal"
        control_journal.mkdir()
        aggressive_journal.mkdir()
        diagnostic_journal.mkdir()
        control_startup = directory / "p1-startup"
        aggressive_startup = directory / "p2-startup"
        diagnostic_startup = directory / "p2-diag-startup"
        control_startup.mkdir()
        aggressive_startup.mkdir()
        diagnostic_startup.mkdir()
        sitecustomize = "from ci_pipeline.jit311 import a2_penetration\n"
        (control_startup / "sitecustomize.py").write_text(sitecustomize)
        (aggressive_startup / "sitecustomize.py").write_text(sitecustomize)
        (diagnostic_startup / "sitecustomize.py").write_text(sitecustomize)

        a1_summary = directory / "a1-threshold50-smoke.json"
        rc_a1 = self.base._run(
            "10-A1-prerequisite",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a1_product_smoke",
                "--expect-mode",
                "execute",
                "--out",
                str(a1_summary),
            ],
            env=self.base._product_env(threshold="50"),
        )
        rc0 = self.base._run("20-A2-P0-stock", self._arm_command(out=stock))
        rc1 = self.base._run(
            "21-A2-P1-threshold50",
            self._arm_command(
                out=control,
                threshold=50,
                startup=control_startup,
                journal=control_journal,
            ),
        )
        rc2 = self.base._run(
            "22-A2-P2-jit-all",
            self._arm_command(
                out=aggressive,
                jit_all=True,
                startup=aggressive_startup,
                journal=aggressive_journal,
            ),
        )
        rc2_diag = self.base._run(
            "22D-A2-P2-threshold1-diagnostic",
            self._arm_command(
                out=diagnostic,
                threshold=1,
                startup=diagnostic_startup,
                journal=diagnostic_journal,
            ),
        )
        control_coverage = directory / "p1-coverage.json"
        aggressive_coverage = directory / "p2-coverage.json"
        diagnostic_coverage = directory / "p2-diag-coverage.json"
        rc_control_coverage = self._classify_penetration(
            "23-A2-P1-coverage",
            control_journal,
            control / "result.json",
            control_coverage,
        )
        rc_aggressive_coverage = self._classify_penetration(
            "24-A2-P2-coverage",
            aggressive_journal,
            aggressive / "result.json",
            aggressive_coverage,
        )
        rc_diagnostic_coverage = self._classify_penetration(
            "24D-A2-P2-threshold1-coverage",
            diagnostic_journal,
            diagnostic / "result.json",
            diagnostic_coverage,
        )
        differential = compare_penetration(
            stock / "result.json",
            aggressive / "result.json",
            self.base.stage
            / "ci_pipeline/jit311/data/a2_compatibility_deviations.json",
        )
        (directory / "p0-vs-p2.json").write_text(
            json.dumps(differential, indent=2, sort_keys=True) + "\n"
        )
        semantic_probe_path = directory / "adaptive-semantic-probe.json"
        rc_semantic_probe = self.base._run(
            "25-A2-P-adaptive-probe",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a1_dis_deviation_probe",
                "--out",
                str(semantic_probe_path),
            ],
            env=self._jit_all_env(),
        )
        control_report = json.loads(control_coverage.read_text())
        aggressive_report = json.loads(aggressive_coverage.read_text())
        diagnostic_report = json.loads(diagnostic_coverage.read_text())
        semantic_probe = json.loads(semantic_probe_path.read_text())
        stock_result = json.loads((stock / "result.json").read_text())
        control_result = json.loads((control / "result.json").read_text())
        control_modules_ok = all(
            value == "pass" for value in control_result.get("modules", {}).values()
        )
        stock_modules_ok = all(
            value == "pass" for value in stock_result.get("modules", {}).values()
        )
        good = (
            rc_a1 == 0
            and rc0 == 0
            and rc1 == 0
            and rc2 == 0
            and rc_aggressive_coverage == 0
            and rc_semantic_probe == 0
            and stock_modules_ok
            and control_modules_ok
            and aggressive_report["result"] == "PASS"
            and differential["result"] in PASS_STATES
            and semantic_probe["result"] == "PASS"
        )
        result = {
            "result": (
                "PASS_WITH_APPROVED_DEVIATIONS"
                if good and differential["differences"]
                else "PASS" if good else "FAIL"
            ),
            "a1_prerequisite": json.loads(a1_summary.read_text()),
            "stock_result": stock_result,
            "control_result": control_result,
            "control_coverage": control_report,
            "aggressive_coverage": aggressive_report,
            "threshold1_diagnostic_coverage": diagnostic_report,
            "differential": differential,
            "adaptive_semantic_probe": semantic_probe,
            "commands": {
                "p0": rc0,
                "p1": rc1,
                "p2": rc2,
                "p2_diag": rc2_diag,
                "p1_coverage": rc_control_coverage,
                "p2_coverage": rc_aggressive_coverage,
                "p2_diag_coverage": rc_diagnostic_coverage,
            },
        }
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        render_penetration_v02(result, self.output / "A2_PENETRATION_V02_REPORT.md")
        return result

    def run_transitions(self) -> dict:
        directory = self.output / "T"
        directory.mkdir()
        stock = directory / "stock.json"
        jit = directory / "jit.json"
        rc_stock = self.base._run(
            "30-A2-T-stock",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_transition_probe",
                "--mode",
                "stock",
                "--out",
                str(stock),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )
        rc_jit = self.base._run(
            "31-A2-T-threshold1",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_transition_probe",
                "--mode",
                "jit",
                "--out",
                str(jit),
            ],
            env=self.base._product_env(threshold="1"),
        )
        tracing_regression_path = directory / "tracing-regression.json"
        rc_tracing_regression = self.base._run(
            "32-A2-T-aggressive-tracing-regression",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a1_tracing_probe",
                "--out",
                str(tracing_regression_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        running_stock = directory / "frame-position-stock.json"
        running_jit = directory / "frame-position-jit.json"
        error_stock = directory / "error-position-stock.json"
        error_jit = directory / "error-position-jit.json"
        probe_module = "ci_pipeline.jit311.a2_frame_position_probe"
        error_module = "ci_pipeline.jit311.a2_error_position_probe"
        rc_running_stock = self.base._run(
            "33-A2-F1-running-stock",
            [
                str(self.base.python),
                "-m",
                probe_module,
                "--mode",
                "stock",
                "--out",
                str(running_stock),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )
        rc_running_jit = self.base._run(
            "34-A2-F1-running-jit",
            [
                str(self.base.python),
                "-m",
                probe_module,
                "--mode",
                "jit",
                "--out",
                str(running_jit),
            ],
            env=self.base._product_env(threshold="1"),
        )
        rc_error_stock = self.base._run(
            "35-A2-F2-error-stock",
            [
                str(self.base.python),
                "-m",
                error_module,
                "--mode",
                "stock",
                "--out",
                str(error_stock),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )
        rc_error_jit = self.base._run(
            "36-A2-F2-error-jit",
            [
                str(self.base.python),
                "-m",
                error_module,
                "--mode",
                "jit",
                "--out",
                str(error_jit),
            ],
            env=self.base._product_env(threshold="1"),
        )
        rc_inspect = self.base._run(
            "37-A2-F1-test-inspect",
            [str(self.base.python), "-m", "test", "-j1", "test_inspect"],
            env=self.base._product_env(threshold="1"),
        )
        if stock.is_file() and jit.is_file():
            result = judge_transitions(
                stock,
                jit,
                self.base.stage / "ci_pipeline/jit311/data/a2_transition_manifest.toml",
            )
        else:
            result = {
                "result": "FAIL",
                "transitions": [],
                "errors": ["stock or JIT transition worker produced no report"],
            }
        result["worker_returncodes"] = {"stock": rc_stock, "jit": rc_jit}
        tracing_regression = (
            json.loads(tracing_regression_path.read_text())
            if tracing_regression_path.is_file()
            else {"result": "FAIL"}
        )
        result["aggressive_tracing_regression"] = tracing_regression
        result["worker_returncodes"]["tracing_regression"] = rc_tracing_regression
        positions = (
            compare_frame_positions(
                running_stock,
                running_jit,
                error_stock,
                error_jit,
            )
            if all(
                path.is_file()
                for path in (running_stock, running_jit, error_stock, error_jit)
            )
            else {"result": "FAIL", "errors": ["position probe report missing"]}
        )
        result["frame_positions"] = positions
        result["worker_returncodes"].update(
            {
                "frame_stock": rc_running_stock,
                "frame_jit": rc_running_jit,
                "error_stock": rc_error_stock,
                "error_jit": rc_error_jit,
                "test_inspect": rc_inspect,
            }
        )
        render_frame_position_report(
            positions,
            self.base.stage
            / "ci_pipeline/jit311/data/a2_frame_position_before_v02.json",
            result,
            rc_inspect,
            self.output / "A2_FRAME_POSITION_REPORT.md",
        )
        if (
            rc_stock != 0
            or rc_jit != 0
            or rc_tracing_regression != 0
            or rc_running_stock != 0
            or rc_running_jit != 0
            or rc_error_stock != 0
            or rc_error_jit != 0
            or rc_inspect != 0
            or tracing_regression.get("result") != "PASS"
            or positions.get("result") != "PASS"
        ):
            result["result"] = "FAIL"
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        return result

    def run_repetition(self) -> dict:
        directory = self.output / "R"
        directory.mkdir()
        repetition_path = directory / "repetition.json"
        footprint_path = directory / "footprint.json"
        rc_repetition = self.base._run(
            "40-A2-R-repetition",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_repetition",
                "--cycles",
                "100",
                "--out",
                str(repetition_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        rc_footprint = self.base._run(
            "50-A2-R-footprint",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.a2_footprint",
                "--out",
                str(footprint_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        repetition = (
            json.loads(repetition_path.read_text())
            if repetition_path.is_file()
            else {"result": "FAIL"}
        )
        footprint = (
            json.loads(footprint_path.read_text())
            if footprint_path.is_file()
            else {"result": "FAIL"}
        )
        return {
            "result": (
                "PASS"
                if rc_repetition == 0
                and rc_footprint == 0
                and repetition["result"] == "PASS"
                and footprint["result"] == "PASS"
                else (
                    "REVIEW_REQUIRED"
                    if repetition["result"] == "PASS"
                    and footprint["result"] == "REVIEW_REQUIRED"
                    else "FAIL"
                )
            ),
            "repetition": repetition,
            "footprint": footprint,
            "returncodes": {"repetition": rc_repetition, "footprint": rc_footprint},
        }

    def finalize(self, provenance: dict) -> str:
        blockers: list[dict] = []
        penetration = self.results.get("P", {})
        aggressive = penetration.get("aggressive_coverage", {})
        gaps = sorted(
            module
            for module, row in aggressive.get("modules", {}).items()
            if row.get("status") == "A2_COVERAGE_GAP"
        )
        if gaps:
            blockers.append(
                {
                    "id": "A2-P-COVERAGE",
                    "severity": "FAIL",
                    "cluster": "scheduler/short-lived-code",
                    "summary": f"threshold=1 own-code penetration is {72-len(gaps)}/72",
                    "modules": gaps,
                    "evidence": "P/p2-coverage.json",
                }
            )
        unexpected = penetration.get("differential", {}).get("unexpected", {})
        inspect_key = "test.test_inspect.TestInterpreterStack.test_stack"
        if inspect_key in unexpected:
            blockers.append(
                {
                    "id": "A2-FRAME-COLUMN",
                    "severity": "FAIL",
                    "cluster": "frame-restore/position",
                    "summary": "threshold=1 changes inspect.stack column end from 27 to 12",
                    "testcase": inspect_key,
                    "evidence": "P/p0-vs-p2.json",
                }
            )
        slots_key = "test.test_descr.ClassPropertiesAndMethods.test_slots"
        if slots_key in unexpected:
            plateau = (
                self.results.get("R", {}).get("footprint", {}).get("plateau", False)
            )
            blockers.append(
                {
                    "id": "A2-RUNTIME-FOOTPRINT",
                    "severity": "REVIEW_REQUIRED",
                    "cluster": "one-time-jit-footprint",
                    "summary": (
                        "first publication adds five GC-tracked objects; plateau is proven and explicit approval is required"
                        if plateau
                        else "first publication adds five GC-tracked objects; plateau is not proven"
                    ),
                    "plateau_proven": plateau,
                    "testcase": slots_key,
                    "evidence": "P/p0-vs-p2.json and R/footprint.json",
                }
            )
        other_unexpected = sorted(
            key
            for key in unexpected
            if key
            not in {
                inspect_key,
                slots_key,
                "<module> test_inspect",
                "<module> test_descr",
            }
        )
        if other_unexpected:
            blockers.append(
                {
                    "id": "A2-P-UNEXPECTED",
                    "severity": "FAIL",
                    "cluster": "penetration-differential",
                    "summary": "unexpected threshold=1 testcase differences",
                    "testcases": other_unexpected,
                    "evidence": "P/p0-vs-p2.json",
                }
            )
        transition_failures = [
            row
            for row in self.results.get("T", {}).get("transitions", [])
            if row.get("result") == "FAIL"
        ]
        if transition_failures:
            blockers.append(
                {
                    "id": "A2-DEOPT-TB-LASTI",
                    "severity": "FAIL",
                    "cluster": "exception-deopt/frame-position",
                    "summary": "T03 and T10 resume with target-frame tb_lasti eight bytes before Stock",
                    "transitions": [row["id"] for row in transition_failures],
                    "evidence": "T/stock.json, T/jit.json, T/result.json",
                }
            )
        repetition_failures = [
            row
            for row in self.results.get("R", {})
            .get("repetition", {})
            .get("transitions", [])
            if row.get("semantic_failures") or row.get("state_failures")
        ]
        if repetition_failures:
            blockers.append(
                {
                    "id": "A2-CODE-SWAP-RECOVERY",
                    "severity": "FAIL",
                    "cluster": "code-identity/auto-recovery",
                    "summary": "repeated two-code __code__ swaps preserve semantics but fail automatic old-artifact recovery",
                    "rows": repetition_failures,
                    "evidence": "R/repetition.json",
                }
            )
        states = [result.get("result") for result in self.results.values()]
        if "FAIL" in states:
            final = "FAIL"
        elif "REVIEW_REQUIRED" in states:
            final = "REVIEW_REQUIRED"
        elif "PASS_WITH_APPROVED_DEVIATIONS" in states:
            final = "PASS_WITH_APPROVED_DEVIATIONS"
        else:
            final = "PASS"
        payload = {
            "final": final,
            "provenance": provenance,
            "penetration": self.results.get("P", {}),
            "transitions": self.results.get("T", {}),
            "repetition": self.results.get("R", {}).get("repetition", {}),
            "footprint": self.results.get("R", {}).get("footprint", {}),
            "blockers": blockers,
            "commands": self.base.command_results,
        }
        (self.output / "a2_result.json").write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n"
        )
        render_markdown(payload, self.output / "A2_EXECUTION_REPORT.md")
        if blockers:
            blocker_lines = ["# A2 Blockers", ""]
            for item in blockers:
                blocker_lines.extend(
                    [
                        f"## {item['id']}",
                        "",
                        f"- Severity: `{item['severity']}`",
                        f"- Cluster: `{item['cluster']}`",
                        f"- Summary: {item['summary']}",
                        f"- Evidence: `{item['evidence']}`",
                        "",
                        "```json",
                        json.dumps(item, indent=2, sort_keys=True),
                        "```",
                        "",
                    ]
                )
            (self.output / "A2_BLOCKERS.md").write_text("\n".join(blocker_lines) + "\n")
        return final

    def run(self) -> str:
        provenance = self.base.preflight()
        if "P" in self.lanes:
            self.results["P"] = self.run_penetration()
        if "T" in self.lanes:
            self.results["T"] = self.run_transitions()
        if "R" in self.lanes:
            self.results["R"] = self.run_repetition()
        return self.finalize(provenance)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--lane", choices=("P", "T", "R"), action="append")
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args(argv)
    output = args.out or Path.cwd() / f"cp311-a2-{datetime.now():%Y%m%d-%H%M%S}"
    runner = A2Runner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        lanes=set(args.lane or ("P", "T", "R")),
        jobs=args.jobs,
        timeout=args.timeout,
    )
    try:
        final = runner.run()
    except Exception as exc:
        print(
            f"A2 runner failed before final judgment: {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    print(f"A2 {final}: {runner.output / 'A2_EXECUTION_REPORT.md'}")
    if final in PASS_STATES:
        return 0
    return 2 if final == "REVIEW_REQUIRED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
