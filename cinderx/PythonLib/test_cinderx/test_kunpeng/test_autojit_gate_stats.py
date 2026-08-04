import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "autojit_gate_stats.py"
CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED = sys.version_info[:3] == (3, 14, 0)
CP3140_AUTOJIT_SKIP_REASON = (
    "CPython 3.14.0 direct calls bypass the AutoJIT classification gate"
)


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
        return run_python_child(
            HELPER,
            case_name,
            cwd=cwd,
            env=env,
            timeout=timeout,
        )

    def test_auto_classify_defaults_import_provider_to_find_and_load(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = self.run_case(
                "import_provider_default",
                cwd=temp,
                env=env,
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

            completed = self.run_case(
                "setup_provider_for_cinderx_init",
                cwd=temp,
                env=env,
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

            completed = self.run_case(
                "import_provider_off",
                cwd=temp,
                env=env,
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

            completed = self.run_case(
                "gate_stats_smoke",
                cwd=temp,
                env=env,
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

            completed = self.run_case(
                "gate_stats_smoke",
                cwd=temp,
                env=env,
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

            completed = self.run_case(
                "compiles_low_roi_functions_at_base",
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
    def test_plugin_compiles_low_loop_object_manipulators_at_base(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = self.run_case(
                "compiles_low_loop_object_manipulators_at_base",
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
    def test_plugin_compiles_self_contained_eafp_predicates(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = self.run_case(
                "compiles_self_contained_eafp_predicates",
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

                completed = run_python_child(
                    script,
                    cwd=temp,
                    env=env,
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

            completed = run_python_child(
                script,
                cwd=temp,
                env=env,
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

            completed = run_python_child(
                script,
                cwd=temp,
                env=env,
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
    def test_plugin_defers_call_only_dispatch_loop(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            env = _plugin_env()

            completed = self.run_case(
                "defers_call_only_dispatch_loop",
                cwd=temp,
                env=env,
            )

            self.assertEqual(
                completed.returncode,
                0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
