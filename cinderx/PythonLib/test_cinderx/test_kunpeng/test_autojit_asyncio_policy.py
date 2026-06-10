import os
import subprocess
import sys
import tempfile
import textwrap
import unittest


class AutoJitAsyncioPolicyTests(unittest.TestCase):
    def run_child(self, source: str) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
            env.pop(name, None)
        env["CINDERX_PLUGIN_ENABLE"] = "1"
        env["PYTHONJITAUTO"] = "auto:2"
        env.pop("PYTHONJITALL", None)

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

    def test_autojit_defers_asyncio_event_loop_helpers(self) -> None:
        self.assert_child_passes(
            """
            import asyncio
            import cinderx.jit as jit

            def straight_compute(a, b):
                return (a + b) * 2

            for value in range(16):
                straight_compute(value, value + 1)
            assert jit.is_jit_compiled(straight_compute), (
                "AutoJIT smoke target did not compile",
                jit.count_interpreted_calls(straight_compute),
            )

            loop = asyncio.new_event_loop()
            try:
                for _ in range(128):
                    loop.call_soon(lambda: None)
            finally:
                loop.close()

            assert not jit.is_jit_compiled(asyncio.BaseEventLoop.call_soon), (
                "BaseEventLoop.call_soon should stay interpreted",
                jit.count_interpreted_calls(asyncio.BaseEventLoop.call_soon),
            )
            assert not jit.is_jit_compiled(asyncio.BaseEventLoop._call_soon), (
                "BaseEventLoop._call_soon should stay interpreted",
                jit.count_interpreted_calls(asyncio.BaseEventLoop._call_soon),
            )
            """
        )


if __name__ == "__main__":
    unittest.main()
