import os
import platform
import subprocess
import sys
import unittest
from pathlib import Path

import cinderx
import cinderx.jit

try:
    import cinderjit
except ImportError:
    cinderjit = None


HELPER = Path(__file__).resolve()
IS_AARCH64 = platform.machine().lower() in {"aarch64", "arm64"}


def _clean_env() -> dict[str, str]:
    env = os.environ.copy()
    for key in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "CINDERX_OSR_ENABLED",
        "CINDERX_PLUGIN_ENABLE",
        "PYTHONJITALL",
        "PYTHONJITAUTO",
        "PYTHONJITDEBUG",
        "PYTHONJITDISABLE",
        "PYTHONJITDUMPASM",
        "PYTHONJITLIGHTWEIGHTFRAME",
    ):
        env.pop(key, None)
    return env


def _run_tls_case(case: str) -> str:
    env = _clean_env()
    env.update(
        {
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITLIGHTWEIGHTFRAME": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    completed = subprocess.run(
        [sys.executable, str(HELPER), "--tls-case", case],
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    output = completed.stdout + completed.stderr
    assert completed.returncode == 0, (
        f"{case}: subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output, output
    return output


def _minimal_jit_target() -> int:
    return 41 + 1


def _run_inline_tls_case(force_fallback: bool = False) -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")

    if force_fallback:
        cinderjit._test_set_thread_state_offset(-1)

    assert cinderx.jit.force_compile(_minimal_jit_target)
    assert cinderx.jit.is_jit_compiled(_minimal_jit_target)
    for _ in range(20):
        assert _minimal_jit_target() == 42
    print("CASE_RESULT minimal_jit_target OK 42")
    cinderx.jit.disassemble(_minimal_jit_target)


class LightweightFramesTests(unittest.TestCase):
    def test_lightweight_frames_api(self) -> None:
        self.assertIsInstance(cinderx.is_lightweight_frames_enabled(), bool)
        self.assertIsInstance(cinderx.jit.is_lightweight_frames_enabled(), bool)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipIf(cinderjit is None, "cinderjit unavailable")
    def test_s4_standard_tls_access_shape_extracts_offset(self) -> None:
        code = [
            0xA9BF7BFD,  # stp x29, x30, [sp, #-16]!
            0x910003FD,  # mov x29, sp
            0xD53BD050,  # mrs x16, tpidr_el0
            0x91404210,  # add x16, x16, #0x10, lsl #12
            0x9107C210,  # add x16, x16, #0x1f0
            0xF9400200,  # ldr x0, [x16]
            0xD65F03C0,  # ret
        ]
        self.assertEqual(
            cinderjit._test_parse_thread_state_prologue(code),
            (0x10 << 12) + 0x1F0,
        )

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipIf(cinderjit is None, "cinderjit unavailable")
    def test_s5_no_prologue_mrs_tls_access_shape_extracts_offset(self) -> None:
        code = [
            0xD53BD050,  # mrs x16, tpidr_el0
            0x91082210,  # add x16, x16, #0x208
            0xF9400200,  # ldr x0, [x16]
            0xD65F03C0,  # ret
        ]
        self.assertEqual(cinderjit._test_parse_thread_state_prologue(code), 0x208)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific JIT code shape")
    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_s6_real_jit_dump_uses_inline_tls_access(self) -> None:
        output = _run_tls_case("inline")
        self.assertIn("tpidr_el0", output)
        self.assertNotIn("_PyThreadState_GetCurrent", output)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific JIT code shape")
    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_s7_inline_tls_minimal_jit_function_executes(self) -> None:
        output = _run_tls_case("inline")
        self.assertIn("CASE_RESULT minimal_jit_target OK 42", output)

    @unittest.skipUnless(IS_AARCH64, "AArch64-specific TLS instruction parser")
    @unittest.skipIf(cinderjit is None, "cinderjit unavailable")
    def test_s8_unrecognized_tls_shape_falls_back_conservatively(self) -> None:
        code = [
            0xD2800000,  # mov x0, #0
            0xD65F03C0,  # ret
        ]
        self.assertIsNone(cinderjit._test_parse_thread_state_prologue(code))
        self.assertIsNone(
            cinderjit._test_parse_thread_state_prologue(
                [
                    0xD53BD050,  # mrs x16, tpidr_el0
                    0x91082210,  # add x16, x16, #0x208
                    0x91004210,  # add x16, x16, #0x10
                ]
            )
        )

        if cinderx.is_lightweight_frames_enabled():
            output = _run_tls_case("fallback")
            self.assertIn("CASE_RESULT minimal_jit_target OK 42", output)
            self.assertNotIn("tpidr_el0", output)

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_getframe_inside_jit_function_materializes_frame(self) -> None:
        def f() -> tuple[bool, bool, bool, bool, bool]:
            frame = sys._getframe(0)
            builtins = __builtins__
            expected_builtins = (
                builtins.__dict__ if hasattr(builtins, "__dict__") else builtins
            )
            return (
                frame.f_globals is globals(),
                frame.f_builtins is expected_builtins,
                frame.f_code is f.__code__,
                isinstance(frame.f_lasti, int),
                isinstance(frame.f_lineno, int),
            )

        self.assertTrue(cinderx.jit.force_compile(f))
        self.assertEqual(f(), (True, True, True, True, True))

    @unittest.skipUnless(
        cinderx.is_lightweight_frames_enabled(),
        "LWF not compiled in",
    )
    def test_traceback_from_jit_function_materializes_frame(self) -> None:
        def f() -> None:
            raise ValueError("from jit")

        self.assertTrue(cinderx.jit.force_compile(f))
        try:
            f()
        except ValueError as caught:
            tb = caught.__traceback__
        else:
            self.fail("ValueError was not raised")

        frames = []
        while tb is not None:
            frames.append((tb.tb_frame.f_code.co_name, tb.tb_frame.f_code.co_filename))
            tb = tb.tb_next

        self.assertIn(("f", __file__), frames)


if __name__ == "__main__":
    if "--tls-case" in sys.argv:
        index = sys.argv.index("--tls-case")
        case = sys.argv[index + 1]
        del sys.argv[index : index + 2]
        _run_inline_tls_case(force_fallback=(case == "fallback"))
        raise SystemExit(0)

    unittest.main()
