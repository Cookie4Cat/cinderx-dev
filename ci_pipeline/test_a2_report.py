import json
from pathlib import Path
import tempfile
import tomllib
import unittest

from ci_pipeline.jit311.a2_penetration import classify
from ci_pipeline.jit311.a2_report import (
    compare_frame_positions,
    compare_penetration,
    judge_transitions,
)
from ci_pipeline.jit311.a2_runner import A2Runner


ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "ci_pipeline/jit311/data"


class A2ReportTest(unittest.TestCase):
    def test_frame_position_comparison_requires_exact_lasti_and_traceback(self):
        observation = {
            "function": "target",
            "f_lasti": 8,
            "f_lineno": 3,
            "co_position": [3, 3, 4, 10],
            "inspect_position": [3, 3, 4, 10],
            "stack_position": [3, 3, 4, 10],
        }
        frame = {
            "function": "target",
            "tb_lasti": 8,
            "f_lasti": 8,
            "tb_lineno": 3,
            "position": [3, 3, 4, 10],
            "opcode": "CALL",
            "opcode_offset": 4,
            "inline_cache_span": 4,
        }
        running_stock = {
            "result": "PASS",
            "rows": [{"case": "CALL", "observation": observation}],
            "entry_ledger_dropped": 0,
        }
        running_jit = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "observation": observation,
                    "machine_entry_proven": True,
                }
            ],
            "entry_ledger_dropped": 0,
        }
        error_stock = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "target_frame": frame,
                    "traceback_frames": [frame],
                }
            ],
            "entry_ledger_dropped": 0,
        }
        error_jit = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "target_frame": frame,
                    "traceback_frames": [frame],
                    "machine_entry_proven": True,
                    "transitions": [{"deopt_reason": "UnhandledException"}],
                    "transition_ledger_dropped": 0,
                }
            ],
            "entry_ledger_dropped": 0,
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            paths = []
            for index, document in enumerate(
                (running_stock, running_jit, error_stock, error_jit)
            ):
                path = directory / f"{index}.json"
                path.write_text(json.dumps(document))
                paths.append(path)
            report = compare_frame_positions(*paths)
            error_jit["rows"][0]["target_frame"] = {**frame, "tb_lasti": 6}
            paths[-1].write_text(json.dumps(error_jit))
            wrong = compare_frame_positions(*paths)
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")
        self.assertTrue(any("traceback position" in item for item in wrong["errors"]))

    def test_a2_p2_uses_jit_all_and_diagnostic_uses_threshold_one(self):
        runner = A2Runner(
            wheel=Path("wheel.whl"),
            source=ROOT,
            output=Path("out"),
            lanes={"P"},
            jobs=16,
            timeout=1200,
        )
        runner.base.stage = ROOT
        jit_all = runner._arm_command(
            out=Path("p2"),
            jit_all=True,
            startup=Path("startup"),
            journal=Path("journal"),
        )
        diagnostic = runner._arm_command(
            out=Path("diag"),
            threshold=1,
            startup=Path("startup"),
            journal=Path("journal"),
        )
        self.assertIn("PYTHONJITALL=1", jit_all)
        self.assertFalse(any(item.startswith("PYTHONJITAUTO=") for item in jit_all))
        self.assertIn("PYTHONJITAUTO=1", diagnostic)
        self.assertNotIn("PYTHONJITALL=1", diagnostic)

    def test_penetration_deviation_requires_fingerprint(self):
        testcase = "test.test_dis.DisTests.test_super_instructions"
        stock = {
            "modules": {"test_dis": "pass"},
            "cases": {testcase: "pass"},
            "diagnostics": {},
        }
        aggressive = {
            "modules": {"test_dis": "fail"},
            "cases": {testcase: "failure"},
            "diagnostics": {
                testcase: "AssertionError RESUME_QUICK LOAD_FAST__LOAD_FAST STORE_FAST__STORE_FAST"
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            left, right = directory / "left.json", directory / "right.json"
            deviations = directory / "deviations.json"
            left.write_text(json.dumps(stock))
            right.write_text(json.dumps(aggressive))
            deviations.write_text(
                json.dumps(
                    {
                        "deviations": [
                            {
                                "testcase": testcase,
                                "stock": "pass",
                                "aggressive": "failure",
                                "fingerprint": [
                                    "AssertionError",
                                    "RESUME_QUICK",
                                    "LOAD_FAST__LOAD_FAST",
                                    "STORE_FAST__STORE_FAST",
                                ],
                            }
                        ]
                    }
                )
            )
            report = compare_penetration(left, right, deviations)
            aggressive["diagnostics"][testcase] = "AssertionError unrelated"
            right.write_text(json.dumps(aggressive))
            wrong = compare_penetration(left, right, deviations)
        self.assertEqual(report["result"], "PASS_WITH_APPROVED_DEVIATIONS")
        self.assertNotIn(testcase, report["unexpected"])
        self.assertEqual(wrong["result"], "FAIL")
        self.assertIn(testcase, wrong["unexpected"])

    def test_transition_judge_requires_stock_and_runtime_reason(self):
        with (DATA / "a2_transition_manifest.toml").open("rb") as stream:
            specs = tomllib.load(stream)["transition"]
        stock_rows = []
        jit_rows = []
        for spec in specs:
            semantic = {"value": spec["id"]}
            stock_rows.append({"id": spec["id"], "semantic": semantic})
            required = spec["requires_transition_reason"]
            recovery = (
                {
                    "policy": spec["recovery"],
                    "interpreter_resume": True,
                    "semantic_correct": True,
                    "stale_machine_entry": False,
                }
                if spec["recovery"]
                in ("interpreter-resume", "interpreter-after-backoff")
                else {"reentered": True}
            )
            jit_rows.append(
                {
                    "id": spec["id"],
                    "semantic": semantic,
                    "pre": {"compiled": True, "machine_entry_proven": True},
                    "transition": {
                        "transition_ledger_dropped": 0,
                        "transition_rows": (
                            [{"deopt_reason": required[0]}] if required else []
                        ),
                    },
                    "recovery": recovery,
                    "result": "PASS",
                }
            )
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stock_path, jit_path = directory / "stock.json", directory / "jit.json"
            stock_path.write_text(json.dumps({"transitions": stock_rows}))
            jit_path.write_text(json.dumps({"transitions": jit_rows}))
            report = judge_transitions(
                stock_path, jit_path, DATA / "a2_transition_manifest.toml"
            )
            jit_rows[0]["transition"]["transition_rows"] = []
            jit_path.write_text(json.dumps({"transitions": jit_rows}))
            wrong = judge_transitions(
                stock_path, jit_path, DATA / "a2_transition_manifest.toml"
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")

    def test_penetration_classifier_requires_all_72_own_code_rows(self):
        targets = [
            line.strip()
            for line in (DATA / "a1_compile_all_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {
                                "machine_code_entries": 1,
                                "compiled_function_creations": 1,
                                "organic_deopt_hits": 0,
                                "forced_deopt_hits": 0,
                            },
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": f"/usr/lib/python3.11/test/{target}.py",
                                "spec_origin": f"/usr/lib/python3.11/test/{target}.py",
                                "package_roots": [],
                            },
                            "entry_ledger": [
                                {
                                    "filename": f"/usr/lib/python3.11/test/{target}.py",
                                    "qualname": "witness",
                                    "firstlineno": 1,
                                    "entries": 1,
                                }
                            ],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "a1_compile_all_modules.txt",
                result_path,
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["counts"]["OWN_CODE_JIT"], 72)

    def test_penetration_classifier_uses_package_ownership(self):
        targets = [
            line.strip()
            for line in (DATA / "a1_compile_all_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                package = f"/usr/lib/python3.11/test/{target}"
                is_package = target == "test_dataclasses"
                filename = (
                    f"{package}/case.py"
                    if is_package
                    else f"/usr/lib/python3.11/test/{target}.py"
                )
                origin = f"{package}/__init__.py" if is_package else filename
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": 1},
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": origin,
                                "spec_origin": origin,
                                "package_roots": [package] if is_package else [],
                            },
                            "entry_ledger": [{"filename": filename, "entries": 1}],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "a1_compile_all_modules.txt",
                result_path,
            )
        self.assertEqual(report["result"], "PASS")
        row = report["modules"]["test_dataclasses"]
        self.assertTrue(row["machine_entry_proven"])
        self.assertEqual(row["own_code_entries"], 1)

    def test_jitall_classifier_accepts_three_fail_closed_states(self):
        targets = [
            line.strip()
            for line in (DATA / "a1_compile_all_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {target: "pass" for target in targets}
            for index, target in enumerate(targets):
                filename = f"/usr/lib/python3.11/test/{target}.py"
                event = {
                    "filename": filename,
                    "qualname": "witness",
                    "count": 1,
                }
                entries = []
                if index < 24:
                    entries = [{"filename": filename, "entries": 1}]
                    events = [{**event, "result": "installed"}]
                elif index < 48:
                    events = [{**event, "result": "installed"}]
                else:
                    events = [{**event, "result": "REFUSE_SHAPE_EXECUTE_SURFACE"}]
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": int(bool(entries))},
                            "observe": {
                                "threshold": 0,
                                "threshold_source": "shared-jit-config",
                                "events": events,
                                "events_dropped": 0,
                            },
                            "ownership": {
                                "module_file": filename,
                                "spec_origin": filename,
                                "package_roots": [],
                            },
                            "entry_ledger": entries,
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            stock_path = directory / "stock.json"
            result_path.write_text(json.dumps({"modules": modules}))
            stock_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "a1_compile_all_modules.txt",
                result_path,
                stock_result_path=stock_path,
                jit_all_contract=True,
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["classified_modules"], 72)
        self.assertEqual(report["counts"]["OWN_CODE_JIT"], 24)
        self.assertEqual(report["counts"]["PUBLISHED_NO_REENTRY"], 24)
        self.assertEqual(report["counts"]["EXPECTED_SAFE_REFUSAL"], 24)
        self.assertEqual(report["counts"].get("COVERAGE_GAP", 0), 0)

    def test_penetration_classifier_rejects_unowned_rows(self):
        targets = [
            line.strip()
            for line in (DATA / "a1_compile_all_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                filename = f"/usr/lib/python3.11/test/{target}.py"
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": 1},
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": filename,
                                "spec_origin": filename,
                                "package_roots": [],
                            },
                            "entry_ledger": [
                                {
                                    "filename": "/usr/lib/python3.11/test/support/__init__.py",
                                    "entries": 1,
                                }
                            ],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "a1_compile_all_modules.txt",
                result_path,
            )
        self.assertEqual(report["counts"]["A2_COVERAGE_GAP"], 72)
        self.assertEqual(report["counts"].get("OWN_CODE_JIT", 0), 0)


if __name__ == "__main__":
    unittest.main()
