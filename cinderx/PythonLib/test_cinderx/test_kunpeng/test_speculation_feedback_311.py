# Copyright (c) Meta Platforms, Inc. and affiliates.
"""CPython 3.11 numeric speculation: float fast paths, per-site fuses."""

import os
import sys
import unittest
from pathlib import Path

from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "speculation_feedback_311.py"


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 feedback")
class SpeculationFeedback311Tests(unittest.TestCase):
    def _run(self, case, **extra):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="0",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            CINDERX_JIT_STARTUP_GRACE_MS="0",
        )
        env.update(extra)
        proc = run_python_child(HELPER, case, env=env, timeout=300)
        self.assertEqual(
            proc.returncode, 0, proc.stdout[-2000:] + proc.stderr[-4000:]
        )
        self.assertIn(f"CASE_OK {case}", proc.stdout)
        return proc

    def test_forced_deopts_do_not_trigger_numeric_feedback(self):
        self._run("forced", CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET="1")

    def test_float_speculation_matches_interpreter(self):
        self._run("semantics")

    def test_failing_float_guard_site_recompiles_to_diamond(self):
        self._run("fuse")

    def test_fuse_retires_by_code_artifact(self):
        self._run("fuse_by_code", PYTHONJITAUTO="4")

    def test_float_site_under_exception_edge_stays_generic(self):
        self._run("exc_edge", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_slow_arm_keeps_float_operand_identity(self):
        self._run("identity")

    def test_fuse_state_follows_code_lifetime_under_batch_compile(self):
        for workers in (1, 8):
            with self.subTest(workers=workers):
                self._run(
                    "lifetime",
                    PYTHONJITMULTITHREADEDCOMPILETEST="1",
                    PYTHONJITBATCHCOMPILEWORKERS=str(workers),
                )


if __name__ == "__main__":
    unittest.main()
