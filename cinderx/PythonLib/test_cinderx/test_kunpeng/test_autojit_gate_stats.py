import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HELPER = Path(__file__).resolve()
CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED = sys.version_info[:3] == (3, 14, 0)
CP3140_AUTOJIT_SKIP_REASON = (
    "CPython 3.14.0 direct calls bypass the AutoJIT classification gate"
)


def install_fake_lib2to3_importer() -> None:
    import importlib.abc
    import importlib.machinery
    import sys

    class FakeLib2to3Loader(importlib.abc.Loader):
        def create_module(self, spec):
            return None

        def exec_module(self, module):
            if module.__name__ == "lib2to3":
                module.__path__ = []
                return
            if module.__name__ == "lib2to3.main":

                def main():
                    return 42

                module.main = main

    class FakeLib2to3Finder(importlib.abc.MetaPathFinder):
        def find_spec(self, fullname, path=None, target=None):
            if fullname == "lib2to3":
                return importlib.machinery.ModuleSpec(
                    fullname,
                    FakeLib2to3Loader(),
                    is_package=True,
                )
            if fullname == "lib2to3.main":
                return importlib.machinery.ModuleSpec(
                    fullname,
                    FakeLib2to3Loader(),
                )
            return None

    sys.meta_path.insert(0, FakeLib2to3Finder())


def case_setup_provider_default() -> None:
    install_fake_lib2to3_importer()
    import lib2to3.main

    assert (
        getattr(
            lib2to3.main.main,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "lib2to3_main"
    )


def case_setup_provider_off() -> None:
    install_fake_lib2to3_importer()
    import lib2to3.main

    assert (
        getattr(
            lib2to3.main.main,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )


def case_multiprocessing_pool_setup_provider_default() -> None:
    import _cinderx
    import cinderx
    import sys
    import types

    observed_depths = []

    module = types.ModuleType("multiprocessing.pool")

    class Pool:
        __module__ = "multiprocessing.pool"

        def __enter__(self):
            observed_depths.append(_cinderx._autojit_setup_depth())
            return self

        def __exit__(self, *exc):
            observed_depths.append(_cinderx._autojit_setup_depth())

        def imap(self, *args):
            observed_depths.append(_cinderx._autojit_setup_depth())
            return iter(())

    class ThreadPool(Pool):
        __module__ = "multiprocessing.pool"

    class IMapIterator:
        def next(self):
            return None

        __next__ = next

    module.Pool = Pool
    module.IMapIterator = IMapIterator
    sys.modules["multiprocessing.pool"] = module

    cinderx._maybe_install_autojit_setup_provider_for_module(
        "multiprocessing.pool"
    )

    assert (
        getattr(
            module.Pool.__enter__,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "multiprocessing_pool"
    )
    assert (
        getattr(
            module.Pool.imap,
            "_cinderx_autojit_setup_provider",
            None,
        )
        == "multiprocessing_pool"
    )
    assert (
        getattr(
            module.IMapIterator.next,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )
    with module.Pool():
        observed_depths.append(_cinderx._autojit_setup_depth())

    assert observed_depths == [1, 1, 1], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    list(module.Pool().imap(None, ()))
    assert observed_depths == [1], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    with ThreadPool():
        observed_depths.append(_cinderx._autojit_setup_depth())

    assert observed_depths == [0, 0, 0], observed_depths
    assert _cinderx._autojit_setup_depth() == 0

    observed_depths.clear()
    list(ThreadPool().imap(None, ()))
    assert observed_depths == [0], observed_depths
    assert _cinderx._autojit_setup_depth() == 0


def case_multiprocessing_pool_setup_provider_off() -> None:
    import cinderx
    import sys
    import types

    module = types.ModuleType("multiprocessing.pool")

    class Pool:
        __module__ = "multiprocessing.pool"

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            pass

        def imap(self, *args):
            return iter(())

    module.Pool = Pool
    sys.modules["multiprocessing.pool"] = module

    cinderx._maybe_install_autojit_setup_provider_for_module(
        "multiprocessing.pool"
    )

    assert (
        getattr(
            module.Pool.__enter__,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )
    assert (
        getattr(
            module.Pool.imap,
            "_cinderx_autojit_setup_provider",
            None,
        )
        is None
    )


def numeric_loop(value):
    total = 0
    for _ in range(8):
        total += value
    return total


def warm_numeric_loop_until_compiled():
    import cinderx.jit as jit

    for _ in range(120):
        numeric_loop(1)

    assert jit.is_jit_compiled(numeric_loop), (
        jit.count_interpreted_calls(numeric_loop)
    )
    return jit


def case_roi_backoff_uncompile() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(32):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_uncompile"] >= 1, stats
    assert stats["roi_recompile"] == 0, stats
    assert stats["roi_frozen"] == 0, stats
    assert not jit.is_jit_compiled(numeric_loop), stats


def case_roi_backoff_default_freeze() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(64):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_frozen"] >= 1, stats
    assert stats["roi_uncompile"] == 0, stats
    assert stats["roi_recompile"] == 0, stats
    assert not jit.is_jit_compiled(numeric_loop), stats


def case_roi_backoff_disabled() -> None:
    import cinderjit

    cinderjit._clear_autojit_gate_stats()
    jit = warm_numeric_loop_until_compiled()

    for _ in range(64):
        numeric_loop(1.5)

    stats = cinderjit._autojit_gate_stats()
    assert stats["roi_frozen"] == 0, stats
    assert stats["roi_uncompile"] == 0, stats
    assert stats["roi_recompile"] == 0, stats
    assert jit.is_jit_compiled(numeric_loop), stats


CASES = {
    "setup_provider_default": case_setup_provider_default,
    "setup_provider_off": case_setup_provider_off,
    "multiprocessing_pool_setup_provider_default": (
        case_multiprocessing_pool_setup_provider_default
    ),
    "multiprocessing_pool_setup_provider_off": (
        case_multiprocessing_pool_setup_provider_off
    ),
    "roi_backoff_uncompile": case_roi_backoff_uncompile,
    "roi_backoff_default_freeze": case_roi_backoff_default_freeze,
    "roi_backoff_disabled": case_roi_backoff_disabled,
}


if "--gate-stats-case" in sys.argv:
    CASES[sys.argv[sys.argv.index("--gate-stats-case") + 1]]()
    sys.exit(0)


def _plugin_env() -> dict[str, str]:
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)

    env["CINDERX_PLUGIN_ENABLE"] = "1"
    env["PYTHONJITAUTO"] = "auto:2"
    # These cases are about classification verdicts, so they run with the
    # held-call budget released: a test process is short-lived by nature and
    # would otherwise never warm up. The budget itself has its own case.
    env["CINDERX_AUTOJIT_LOWROI_WARM_CALLS"] = "0"
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
    def run_case(
        self,
        case_name: str,
        *,
        cwd: str,
        env: dict[str, str],
        timeout: int = 60,
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(HELPER), "--gate-stats-case", case_name],
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
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
            completed = self.run_case(
                "setup_provider_default",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )

    def test_auto_classify_defaults_setup_provider_to_multiprocessing_pool(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            completed = self.run_case(
                "multiprocessing_pool_setup_provider_default",
                cwd=temp,
                env=env,
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

    def test_plugin_without_auto_classify_keeps_multiprocessing_pool_provider_off(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env_without_auto_classify()
            completed = self.run_case(
                "multiprocessing_pool_setup_provider_off",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )

    def test_plugin_without_auto_classify_keeps_setup_provider_off(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env_without_auto_classify()
            completed = self.run_case(
                "setup_provider_off",
                cwd=temp,
                env=env,
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

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_roi_backoff_uncompiles_deopt_storm(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"
            env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "1"
            env["CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET"] = "8"
            env["CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS"] = "2"
            completed = self.run_case(
                "roi_backoff_uncompile",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_roi_backoff_defaults_freeze_first_deopt_storm(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"
            completed = self.run_case(
                "roi_backoff_default_freeze",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_roi_backoff_env_zero_disables_backoff(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["PYTHONJITAUTO"] = "auto:100"
            env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "0"
            completed = self.run_case(
                "roi_backoff_disabled",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_compiles_low_roi_functions_at_base(self) -> None:
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
                    "assert trivial_stats['classified_defer_freeze'] == 0, trivial_stats\n"
                    "assert jit.is_jit_compiled(trivial), trivial_stats\n"
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
                    "assert dispatch_stats['classified_defer_freeze'] == 0, dispatch_stats\n"
                    "assert jit.is_jit_compiled(dispatch), dispatch_stats\n",
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

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_compiles_low_loop_object_manipulators_at_base(self) -> None:
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
                    "assert stats['classified_defer_freeze'] == 0, stats\n"
                    "assert jit.is_jit_compiled(write_value), stats\n",
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

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_compiles_self_contained_eafp_predicates(self) -> None:
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
                    "assert jit.is_jit_compiled(logging.Logger.isEnabledFor), (\n"
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

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_low_roi_release_waits_for_held_call_budget(self) -> None:
        # A process that never accumulates the budget keeps the deferral, so
        # short-lived interpreter invocations stay as cheap as before the
        # release; one that does accumulate it compiles the same shape. The
        # third case pins the second gate: a single shape that alone reaches
        # the 65535 hold threshold compiles on its own evidence even when the
        # process-level budget never releases.
        script = Path(__file__).with_name("held_call_budget_helper.py")
        with tempfile.TemporaryDirectory() as temp:
            for calls, want, budget in (
                (64, "interpreted", "1024"),
                (5000, "compiled", "1024"),
                (140000, "compiled", "1000000000"),
            ):
                env = _plugin_env()
                env["CINDERX_AUTOJIT_LOWROI_WARM_CALLS"] = budget
                env["CALLS"] = str(calls)
                env["WANT"] = want

                completed = subprocess.run(
                    [sys.executable, str(script)],
                    cwd=temp,
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )

                self.assertEqual(
                    completed.returncode,
                    0,
                    f"calls={calls} want={want} budget={budget}"
                    f"\nstdout:\n{completed.stdout}"
                    f"\nstderr:\n{completed.stderr}",
                )

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    @unittest.skipUnless(hasattr(os, "fork"), "requires os.fork")
    def test_plugin_forked_child_reproves_held_call_budget(self) -> None:
        # The budget's evidence is per-process execution. A forked child has
        # executed nothing, so it must not inherit the parent's release:
        # disposable pool workers otherwise compile eagerly and pay bursts
        # they can never amortize. Compiled code itself is inherited.
        script = Path(__file__).with_name("fork_budget_helper.py")
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["CINDERX_AUTOJIT_LOWROI_WARM_CALLS"] = "1024"

            completed = subprocess.run(
                [sys.executable, str(script)],
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

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_plugin_dedup_uncompile_demotes_donor(self) -> None:
        # Lifecycle: promote a donor, gut its artifact via force_uncompile
        # (the same path ROI backoff takes), then verify a fresh twin does
        # not get the cleared artifact re-installed -- it recompiles cleanly
        # and the entry can be promoted again.
        script = Path(__file__).with_name("dedup_uncompile_helper.py")
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()
            env["CINDERX_AUTOJIT_CODE_DEDUP"] = "1"

            completed = subprocess.run(
                [sys.executable, str(script)],
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
