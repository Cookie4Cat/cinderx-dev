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
    env.pop("PYTHONJITALL", None)
    env.pop("CINDERX_AUTOJIT_IMPORT_PROVIDER", None)
    return env


def _plugin_env_without_auto_classify() -> dict[str, str]:
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    for name in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "PYTHONJITDISABLE",
        "PYTHONJITALL",
        "PYTHONJITAUTO",
        "CINDERX_AUTOJIT_IMPORT_PROVIDER",
    ):
        env.pop(name, None)

    env["CINDERX_PLUGIN_ENABLE"] = "1"
    return env


class AutoJitGateStatsDumpTests(unittest.TestCase):
    def test_auto_classify_defaults_import_provider_to_find_and_load(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import sys\n"
                    "bootstrap = sys.modules['importlib._bootstrap']\n"
                    "assert getattr(\n"
                    "    bootstrap._find_and_load,\n"
                    "    '_cinderx_autojit_import_provider',\n"
                    "    None,\n"
                    ") == 'find_and_load'\n",
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

    def test_plugin_without_auto_classify_keeps_import_provider_off(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env_without_auto_classify()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import sys\n"
                    "bootstrap = sys.modules['importlib._bootstrap']\n"
                    "assert getattr(\n"
                    "    bootstrap._find_and_load,\n"
                    "    '_cinderx_autojit_import_provider',\n"
                    "    None,\n"
                    ") is None\n",
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

    def test_plugin_freezes_low_roi_functions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "def trivial(value):\n"
                    "    return value\n"
                    "\n"
                    "for value in range(20):\n"
                    "    trivial(value)\n"
                    "\n"
                    "trivial_stats = cinderjit._autojit_gate_stats()\n"
                    "assert trivial_stats['global_threshold_return'] >= 1, trivial_stats\n"
                    "assert trivial_stats['classified_defer_freeze'] >= 1, trivial_stats\n"
                    "assert trivial_stats['classified_warmup_return'] == 0, trivial_stats\n"
                    "assert trivial_stats['forced_compile'] == 0, trivial_stats\n"
                    "assert jit.count_interpreted_calls(trivial) <= 2\n"
                    "assert not jit.is_jit_compiled(trivial)\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "def identity(value):\n"
                    "    return value\n"
                    "\n"
                    "def dispatch(func, value):\n"
                    "    return func(value)\n"
                    "\n"
                    "for value in range(2000):\n"
                    "    dispatch(identity, value)\n"
                    "\n"
                    "dispatch_stats = cinderjit._autojit_gate_stats()\n"
                    "assert dispatch_stats['global_threshold_return'] >= 1, dispatch_stats\n"
                    "assert dispatch_stats['classified_defer_freeze'] >= 1, dispatch_stats\n"
                    "assert dispatch_stats['classified_warmup_return'] == 0, dispatch_stats\n"
                    "assert dispatch_stats['forced_compile'] == 0, dispatch_stats\n"
                    "assert jit.count_interpreted_calls(dispatch) <= 2\n"
                    "assert not jit.is_jit_compiled(dispatch)\n",
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


if __name__ == "__main__":
    unittest.main()
