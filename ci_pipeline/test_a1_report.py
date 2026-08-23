import json
from pathlib import Path
import tempfile
import unittest

from ci_pipeline.jit311.a1_report import (
    classify_compile_all,
    compare_with_deviations,
)


ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "ci_pipeline/jit311/data"


class A1ReportTest(unittest.TestCase):
    def test_compile_all_classifies_the_frozen_72_modules(self):
        targets = [
            line.strip()
            for line in (DATA / "a1_compile_all_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        self.assertEqual(len(targets), 72)
        with tempfile.TemporaryDirectory() as temporary:
            journal = Path(temporary)
            events = []
            for index, target in enumerate(targets, 1):
                full = "test." + target
                events.extend(
                    [
                        {
                            "type": "compile",
                            "filename": f"/{target}.py",
                            "firstlineno": index,
                            "qualname": "test_method",
                            "status": "compiled",
                            "reason": None,
                        },
                        {
                            "type": "module-scan",
                            "module": full,
                            "discovered": 1,
                            "statuses": {"compiled": 1},
                            "reasons": {},
                        },
                        {
                            "type": "test-call",
                            "target_module": full,
                            "machine_entries_delta": 1,
                        },
                    ]
                )
            (journal / "1.jsonl").write_text(
                "".join(json.dumps(event) + "\n" for event in events)
            )
            report = classify_compile_all(
                journal,
                DATA / "a1_compile_all_modules.txt",
                DATA / "a1_compile_all_capabilities.toml",
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["module_counts"]["JIT_EXECUTED"], 72)
        self.assertEqual(report["module_counts"]["UNCOVERED"], 0)
        self.assertEqual(report["functions"]["unknown_refusal"], 0)

    def test_unknown_refusal_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            journal = Path(temporary)
            (journal / "1.jsonl").write_text(
                json.dumps(
                    {
                        "type": "compile",
                        "filename": "/unknown.py",
                        "firstlineno": 1,
                        "qualname": "unknown",
                        "status": "unknown-refusal",
                        "reason": None,
                    }
                )
                + "\n"
            )
            report = classify_compile_all(
                journal,
                DATA / "a1_compile_all_modules.txt",
                DATA / "a1_compile_all_capabilities.toml",
            )
        self.assertEqual(report["result"], "FAIL")
        self.assertEqual(report["functions"]["unknown_refusal"], 1)

    def test_exact_test_dis_deviations_are_lane_scoped(self):
        cases = {
            "test.test_dis.DisTests.test_loop_quicken": "pass",
            "test.test_dis.DisTests.test_super_instructions": "pass",
            "test.test_dis.DisWithFileTests.test_loop_quicken": "pass",
            "test.test_dis.DisWithFileTests.test_super_instructions": "pass",
        }
        stock = {"modules": {"test_dis": "pass"}, "cases": cases, "diagnostics": {}}
        execute = {
            "modules": {"test_dis": "fail"},
            "cases": {key: "failure" for key in cases},
            "diagnostics": {},
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stock_path, execute_path = directory / "stock.json", directory / "execute.json"
            stock_path.write_text(json.dumps(stock))
            execute_path.write_text(json.dumps(execute))
            report = compare_with_deviations(
                stock_path,
                execute_path,
                DATA / "a1_compatibility_deviations.json",
            )
        self.assertEqual(report["result"], "PASS_WITH_APPROVED_DEVIATIONS")
        self.assertFalse(report["unexpected"])
        self.assertFalse(report["stale_baseline"])
        self.assertEqual(len(report["approved_deviations"]), 4)

    def test_stale_deviation_requires_review(self):
        result = {"modules": {"test_dis": "pass"}, "cases": {}, "diagnostics": {}}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            left, right = directory / "left.json", directory / "right.json"
            left.write_text(json.dumps(result))
            right.write_text(json.dumps(result))
            report = compare_with_deviations(
                left, right, DATA / "a1_compatibility_deviations.json"
            )
        self.assertEqual(report["result"], "REVIEW_REQUIRED")
        self.assertEqual(len(report["stale_baseline"]), 4)


if __name__ == "__main__":
    unittest.main()
