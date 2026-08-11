import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "autojit_policy.py"
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
    env.pop("PYTHONJITALL", None)
    return env


class AutoJitPolicyTests(unittest.TestCase):
    def run_case(
        self,
        case_name: str,
        *,
        env_overrides: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = _plugin_env()
        if env_overrides:
            env.update(env_overrides)
        with tempfile.TemporaryDirectory() as tmpdir:
            return run_python_child(
                HELPER,
                case_name.replace("_", "-"),
                cwd=tmpdir,
                env=env,
                timeout=60,
            )

    def assert_case_passes(
        self,
        case_name: str,
        *,
        env_overrides: dict[str, str] | None = None,
    ) -> None:
        completed = self.run_case(case_name, env_overrides=env_overrides)
        self.assertEqual(
            completed.returncode,
            0,
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_autojit_defers_asyncio_event_loop_helpers(self) -> None:
        self.assert_case_passes("asyncio_event_loop_helpers")

    @unittest.skipIf(
        CP3140_AUTOJIT_DIRECT_CALL_GATE_UNSUPPORTED,
        CP3140_AUTOJIT_SKIP_REASON,
    )
    def test_autojit_defers_deepcopy_tuple_and_keep_alive(self) -> None:
        self.assert_case_passes(
            "deepcopy_exception_helpers",
            env_overrides={"CINDERX_AUTOJIT_IMPORT_PROVIDER": "off"},
        )


if __name__ == "__main__":
    unittest.main()
