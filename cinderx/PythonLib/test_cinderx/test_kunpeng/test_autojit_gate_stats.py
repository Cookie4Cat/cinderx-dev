import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def _plugin_env() -> dict[str, str]:
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)

    env["CINDERX_PLUGIN_ENABLE"] = "1"
    env["PYTHONJITAUTO"] = "auto:2"
    return env


class AutoJitGateStatsDumpTests(unittest.TestCase):
    def test_plugin_dumps_gate_stats_when_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            stats_path = Path(temp) / "gate-stats.jsonl"
            env = _plugin_env()
            env["CINDERX_AUTOJIT_GATE_STATS"] = "1"
            env["CINDERX_AUTOJIT_GATE_STATS_FILE"] = str(stats_path)

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "def target(x):\n"
                    "    return x + 1\n"
                    "for i in range(5):\n"
                    "    target(i)\n",
                ],
                cwd=temp,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            self.assertTrue(
                stats_path.exists(),
                f"gate stats file was not written\nstderr:\n{completed.stderr}",
            )
            lines = stats_path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1, lines)

            payload = json.loads(lines[0])
            self.assertIsInstance(payload["pid"], int)
            stats = payload["stats"]
            self.assertGreaterEqual(stats["jit_vectorcall"], 1, stats)
            self.assertGreaterEqual(stats["global_threshold_return"], 1, stats)
            self.assertIn("forced_compile", stats)

    def test_plugin_does_not_dump_gate_stats_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            stats_path = Path(temp) / "gate-stats.jsonl"
            env = _plugin_env()
            env["CINDERX_AUTOJIT_GATE_STATS_FILE"] = str(stats_path)

            completed = subprocess.run(
                [sys.executable, "-c", "def target():\n    return 1\ntarget()\n"],
                cwd=temp,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            self.assertFalse(stats_path.exists())


if __name__ == "__main__":
    unittest.main()
