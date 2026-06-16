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


def _run_compile_smoke():
    import cinderx.jit as jit

    def straight_compute(a, b):
        return (a + b) * 2

    for value in range(16):
        straight_compute(value, value + 1)
    assert jit.is_jit_compiled(straight_compute), (
        "AutoJIT smoke target did not compile",
        jit.count_interpreted_calls(straight_compute),
    )
    return jit


def case_asyncio_event_loop_helpers():
    import asyncio

    jit = _run_compile_smoke()
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


def case_deepcopy_exception_helpers():
    import copy

    jit = _run_compile_smoke()
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


CASES = {
    "asyncio_event_loop_helpers": case_asyncio_event_loop_helpers,
    "deepcopy_exception_helpers": case_deepcopy_exception_helpers,
}


if "--case" in sys.argv:
    CASES[sys.argv[sys.argv.index("--case") + 1]]()
    sys.exit(0)


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
            return subprocess.run(
                [sys.executable, str(HELPER), "--case", case_name],
                cwd=tmpdir,
                env=env,
                capture_output=True,
                text=True,
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
