import os
import subprocess
import sys
import tempfile
import textwrap
import unittest


class AutoJitExceptionPolicyTests(unittest.TestCase):
    def run_child(self, source: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
            env.pop(name, None)
        env["CINDERX_PLUGIN_ENABLE"] = "1"
        env["PYTHONJITAUTO"] = "auto:2"
        env["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "off"
        with tempfile.TemporaryDirectory() as tmpdir:
            return subprocess.run(
                [sys.executable, "-c", textwrap.dedent(source)],
                cwd=tmpdir,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
            )

    def assert_child_passes(self, source: str) -> None:
        completed = self.run_child(source)
        self.assertEqual(
            completed.returncode,
            0,
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )

    def test_autojit_defers_deepcopy_tuple_and_keep_alive(self) -> None:
        self.assert_child_passes(
            """
            import copy
            import cinderx.jit as jit

            def straight_compute(a, b):
                return (a + b) * 2

            for value in range(16):
                straight_compute(value, value + 1)
            assert jit.is_jit_compiled(straight_compute), (
                "AutoJIT smoke target did not compile",
                jit.count_interpreted_calls(straight_compute),
            )

            for _ in range(2048):
                copy._deepcopy_tuple((object(),), {})
            assert not jit.is_jit_compiled(copy._deepcopy_tuple), (
                "_deepcopy_tuple should stay deferred",
                jit.count_interpreted_calls(copy._deepcopy_tuple),
            )

            memo = {}
            for _ in range(2048):
                copy._keep_alive(object(), memo)
            assert not jit.is_jit_compiled(copy._keep_alive), (
                "_keep_alive should keep the existing RiskDefer behavior",
                jit.count_interpreted_calls(copy._keep_alive),
            )
            """
        )


if __name__ == "__main__":
    unittest.main()
