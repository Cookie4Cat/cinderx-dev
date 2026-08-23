import json
from pathlib import Path
import tempfile
import tomllib
import unittest

from ci_pipeline.jit311.a2_penetration import classify
from ci_pipeline.jit311.a2_report import compare_penetration, judge_transitions


ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "ci_pipeline/jit311/data"


class A2ReportTest(unittest.TestCase):
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
            left.write_text(json.dumps(stock))
            right.write_text(json.dumps(aggressive))
            report = compare_penetration(
                left, right, DATA / "a2_compatibility_deviations.json"
            )
            aggressive["diagnostics"][testcase] = "AssertionError unrelated"
            right.write_text(json.dumps(aggressive))
            wrong = compare_penetration(
                left, right, DATA / "a2_compatibility_deviations.json"
            )
        self.assertEqual(report["result"], "REVIEW_REQUIRED")
        # The other approved superinstruction case is stale in this synthetic
        # one-case input; the matching case itself must not be unexpected.
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
                {"interpreter_resume": True}
                if spec["recovery"] == "interpreter-resume"
                else {"reentered": True}
            )
            jit_rows.append(
                {
                    "id": spec["id"],
                    "semantic": semantic,
                    "pre": {"compiled": True, "machine_entry_proven": True},
                    "transition": {
                        "transition_ledger_dropped": 0,
                        "transition_rows": [
                            {"deopt_reason": required[0]}
                        ]
                        if required
                        else [],
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


if __name__ == "__main__":
    unittest.main()
