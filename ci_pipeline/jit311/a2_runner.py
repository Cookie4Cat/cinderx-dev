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
    compare_penetration,
    judge_transitions,
    render_markdown,
)


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
        startup: Path | None = None,
        journal: Path | None = None,
    ) -> list[str]:
        targets = [
            line.strip()
            for line in (
                self.base.stage
                / "ci_pipeline/jit311/data/a1_compile_all_modules.txt"
            ).read_text().splitlines()
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
                f"PYTHONJITAUTO={threshold}",
                "--env",
                "PYTHONJITGENERATOR=1",
            ]
        command += ["--tests", *targets]
        return command

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
        aggressive = directory / "p2-threshold1"
        control_journal = directory / "p1-journal"
        aggressive_journal = directory / "p2-journal"
        control_journal.mkdir()
        aggressive_journal.mkdir()
        control_startup = directory / "p1-startup"
        aggressive_startup = directory / "p2-startup"
        control_startup.mkdir()
        aggressive_startup.mkdir()
        sitecustomize = "from ci_pipeline.jit311 import a2_penetration\n"
        (control_startup / "sitecustomize.py").write_text(sitecustomize)
        (aggressive_startup / "sitecustomize.py").write_text(sitecustomize)

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
            "22-A2-P2-threshold1",
            self._arm_command(
                out=aggressive,
                threshold=1,
                startup=aggressive_startup,
                journal=aggressive_journal,
            ),
        )
        control_coverage = directory / "p1-coverage.json"
        aggressive_coverage = directory / "p2-coverage.json"
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
            env=self.base._product_env(threshold="1"),
        )
        control_report = json.loads(control_coverage.read_text())
        aggressive_report = json.loads(aggressive_coverage.read_text())
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
            "differential": differential,
            "adaptive_semantic_probe": semantic_probe,
            "commands": {
                "p0": rc0,
                "p1": rc1,
                "p2": rc2,
                "p1_coverage": rc_control_coverage,
                "p2_coverage": rc_aggressive_coverage,
            },
        }
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
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
        if stock.is_file() and jit.is_file():
            result = judge_transitions(
                stock,
                jit,
                self.base.stage
                / "ci_pipeline/jit311/data/a2_transition_manifest.toml",
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
        if (
            rc_stock != 0
            or rc_jit != 0
            or rc_tracing_regression != 0
            or tracing_regression.get("result") != "PASS"
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
        repetition = json.loads(repetition_path.read_text()) if repetition_path.is_file() else {"result": "FAIL"}
        footprint = json.loads(footprint_path.read_text()) if footprint_path.is_file() else {"result": "FAIL"}
        return {
            "result": (
                "PASS"
                if rc_repetition == 0
                and rc_footprint == 0
                and repetition["result"] == "PASS"
                and footprint["result"] == "PASS"
                else "REVIEW_REQUIRED"
                if repetition["result"] == "PASS"
                and footprint["result"] == "REVIEW_REQUIRED"
                else "FAIL"
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
            plateau = self.results.get("R", {}).get("footprint", {}).get(
                "plateau", False
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
            (self.output / "A2_BLOCKERS.md").write_text(
                "\n".join(blocker_lines) + "\n"
            )
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
