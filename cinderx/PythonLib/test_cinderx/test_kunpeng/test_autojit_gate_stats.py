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
    env.pop("CINDERX_AUTOJIT_SETUP_PROVIDER", None)
    env.pop("CINDERX_AUTOJIT_ROI_BACKOFF", None)
    env.pop("CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET", None)
    env.pop("CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS", None)
    env.pop("CINDERX_AUTOJIT_ROI_REWARM_FACTOR", None)
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
        "CINDERX_AUTOJIT_SETUP_PROVIDER",
        "CINDERX_AUTOJIT_ROI_BACKOFF",
        "CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET",
        "CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS",
        "CINDERX_AUTOJIT_ROI_REWARM_FACTOR",
    ):
        env.pop(name, None)

    env["CINDERX_PLUGIN_ENABLE"] = "1"
    return env


class AutoJitGateStatsDumpTests(unittest.TestCase):
    _FAKE_LIB2TO3_IMPORTER = (
        "import importlib.abc\n"
        "import importlib.machinery\n"
        "import sys\n"
        "\n"
        "class FakeLib2to3Loader(importlib.abc.Loader):\n"
        "    def create_module(self, spec):\n"
        "        return None\n"
        "\n"
        "    def exec_module(self, module):\n"
        "        if module.__name__ == 'lib2to3':\n"
        "            module.__path__ = []\n"
        "            return\n"
        "        if module.__name__ == 'lib2to3.main':\n"
        "            def main():\n"
        "                return 42\n"
        "            module.main = main\n"
        "\n"
        "class FakeLib2to3Finder(importlib.abc.MetaPathFinder):\n"
        "    def find_spec(self, fullname, path=None, target=None):\n"
        "        if fullname == 'lib2to3':\n"
        "            return importlib.machinery.ModuleSpec(\n"
        "                fullname,\n"
        "                FakeLib2to3Loader(),\n"
        "                is_package=True,\n"
        "            )\n"
        "        if fullname == 'lib2to3.main':\n"
        "            return importlib.machinery.ModuleSpec(\n"
        "                fullname,\n"
        "                FakeLib2to3Loader(),\n"
        "            )\n"
        "        return None\n"
        "\n"
        "sys.meta_path.insert(0, FakeLib2to3Finder())\n"
    )

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

    def test_auto_classify_defaults_setup_provider_to_lib2to3_main(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    (
                        self._FAKE_LIB2TO3_IMPORTER
                        + "import lib2to3.main\n"
                        "assert getattr(\n"
                        "    lib2to3.main.main,\n"
                        "    '_cinderx_autojit_setup_provider',\n"
                        "    None,\n"
                        ") == 'lib2to3_main'\n"
                    ),
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

    def test_auto_classify_defaults_setup_provider_for_cinderx_init(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import sys\n"
                    "import types\n"
                    "import cinderx\n"
                    "module = types.ModuleType('lib2to3.main')\n"
                    "def main():\n"
                    "    return 42\n"
                    "module.main = main\n"
                    "sys.modules['lib2to3.main'] = module\n"
                    "cinderx._maybe_install_autojit_setup_provider_for_module(\n"
                    "    'lib2to3.main'\n"
                    ")\n"
                    "assert getattr(\n"
                    "    module.main,\n"
                    "    '_cinderx_autojit_setup_provider',\n"
                    "    None,\n"
                    ") == 'lib2to3_main'\n",
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

    def test_plugin_without_auto_classify_keeps_setup_provider_off(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env_without_auto_classify()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    (
                        self._FAKE_LIB2TO3_IMPORTER
                        + "import lib2to3.main\n"
                        "assert getattr(\n"
                        "    lib2to3.main.main,\n"
                        "    '_cinderx_autojit_setup_provider',\n"
                        "    None,\n"
                        ") is None\n"
                    ),
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

    def test_plugin_roi_backoff_uncompiles_deopt_storm(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"
            env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "1"
            env["CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET"] = "8"
            env["CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS"] = "2"

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "def numeric_loop(value):\n"
                    "    total = 0\n"
                    "    for _ in range(8):\n"
                    "        total += value\n"
                    "    return total\n"
                    "\n"
                    "for _ in range(120):\n"
                    "    numeric_loop(1)\n"
                    "\n"
                    "assert jit.is_jit_compiled(numeric_loop), (\n"
                    "    jit.count_interpreted_calls(numeric_loop)\n"
                    ")\n"
                    "\n"
                    "for _ in range(32):\n"
                    "    numeric_loop(1.5)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['roi_uncompile'] >= 1, stats\n"
                    "assert stats['roi_recompile'] == 0, stats\n"
                    "assert stats['roi_frozen'] == 0, stats\n"
                    "assert not jit.is_jit_compiled(numeric_loop), stats\n",
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

    def test_plugin_roi_backoff_defaults_freeze_first_deopt_storm(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "def numeric_loop(value):\n"
                    "    total = 0\n"
                    "    for _ in range(8):\n"
                    "        total += value\n"
                    "    return total\n"
                    "\n"
                    "for _ in range(120):\n"
                    "    numeric_loop(1)\n"
                    "\n"
                    "assert jit.is_jit_compiled(numeric_loop), (\n"
                    "    jit.count_interpreted_calls(numeric_loop)\n"
                    ")\n"
                    "\n"
                    "for _ in range(64):\n"
                    "    numeric_loop(1.5)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['roi_frozen'] >= 1, stats\n"
                    "assert stats['roi_uncompile'] == 0, stats\n"
                    "assert stats['roi_recompile'] == 0, stats\n"
                    "assert not jit.is_jit_compiled(numeric_loop), stats\n",
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

    def test_plugin_roi_backoff_env_zero_disables_backoff(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"
            env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "0"

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "def numeric_loop(value):\n"
                    "    total = 0\n"
                    "    for _ in range(8):\n"
                    "        total += value\n"
                    "    return total\n"
                    "\n"
                    "for _ in range(120):\n"
                    "    numeric_loop(1)\n"
                    "\n"
                    "assert jit.is_jit_compiled(numeric_loop), (\n"
                    "    jit.count_interpreted_calls(numeric_loop)\n"
                    ")\n"
                    "\n"
                    "for _ in range(64):\n"
                    "    numeric_loop(1.5)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['roi_frozen'] == 0, stats\n"
                    "assert stats['roi_uncompile'] == 0, stats\n"
                    "assert stats['roi_recompile'] == 0, stats\n"
                    "assert jit.is_jit_compiled(numeric_loop), stats\n",
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

    def test_plugin_freezes_low_loop_object_manipulators(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "class Payload:\n"
                    "    def __init__(self):\n"
                    "        self.value = 42\n"
                    "\n"
                    "def write_value(obj, value):\n"
                    "    obj.value = value\n"
                    "    return obj.value\n"
                    "\n"
                    "payload = Payload()\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "\n"
                    "for value in range(20):\n"
                    "    write_value(payload, value)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['global_threshold_return'] >= 1, stats\n"
                    "assert stats['classified_defer_freeze'] >= 1, stats\n"
                    "assert stats['classified_warmup_return'] == 0, stats\n"
                    "assert stats['forced_compile'] == 0, stats\n"
                    "assert jit.count_interpreted_calls(write_value) <= 2\n"
                    "assert not jit.is_jit_compiled(write_value)\n",
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

    def test_plugin_defers_logging_disabled_fast_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import logging\n"
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "logger = logging.getLogger('cinderx.autojit.logging')\n"
                    "logger.handlers[:] = []\n"
                    "logger.setLevel(logging.WARNING)\n"
                    "logger.propagate = False\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "for _ in range(16):\n"
                    "    assert not logger.isEnabledFor(logging.DEBUG)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['global_threshold_return'] >= 1, stats\n"
                    "assert stats['classified_defer_freeze'] >= 1, stats\n"
                    "assert stats['forced_compile'] == 0, stats\n"
                    "assert not jit.is_jit_compiled(logging.Logger.isEnabledFor), (\n"
                    "    stats,\n"
                    "    jit.count_interpreted_calls(logging.Logger.isEnabledFor),\n"
                    ")\n",
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

    def test_plugin_defers_call_only_dispatch_loop(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    "import cinderjit\n"
                    "import cinderx.jit as jit\n"
                    "\n"
                    "class Target:\n"
                    "    def debug(self, msg):\n"
                    "        return None\n"
                    "\n"
                    "def call_only_loop(target, msg, loops):\n"
                    "    for _ in range(loops):\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "        target.debug(msg)\n"
                    "\n"
                    "target = Target()\n"
                    "for _ in range(8):\n"
                    "    target.debug('x')\n"
                    "\n"
                    "cinderjit._clear_autojit_gate_stats()\n"
                    "for _ in range(8):\n"
                    "    call_only_loop(target, 'x', 4)\n"
                    "\n"
                    "stats = cinderjit._autojit_gate_stats()\n"
                    "assert stats['classified_defer_freeze'] >= 1, stats\n"
                    "assert not jit.is_jit_compiled(call_only_loop), (\n"
                    "    stats,\n"
                    "    jit.count_interpreted_calls(call_only_loop),\n"
                    ")\n",
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
