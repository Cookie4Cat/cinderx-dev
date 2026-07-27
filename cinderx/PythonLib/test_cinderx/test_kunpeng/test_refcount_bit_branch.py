import os
import platform
import re
import subprocess
import sys
import unittest
from pathlib import Path

import cinderx.jit


HELPER = Path(__file__).resolve()
IS_AARCH64 = platform.machine().lower() in {"aarch64", "arm64"}


def _refcount_target(value: object) -> object:
    values = [value, value]
    wrapped = {"values": values}
    return wrapped["values"][0]


def _run_jit_case() -> None:
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if not cinderx.jit.force_compile(_refcount_target):
        raise RuntimeError("could not compile refcount target")

    marker = object()
    before = sys.getrefcount(marker)
    for _ in range(1000):
        if _refcount_target(marker) is not marker:
            raise AssertionError("JIT result did not preserve object identity")
    after = sys.getrefcount(marker)
    if after != before:
        raise AssertionError(f"refcount changed: before={before}, after={after}")

    immortal_before = sys.getrefcount(None)
    for _ in range(1000):
        if _refcount_target(None) is not None:
            raise AssertionError("JIT result did not preserve immortal singleton")
    immortal_after = sys.getrefcount(None)
    if immortal_after != immortal_before:
        raise AssertionError(
            "immortal refcount changed: "
            f"before={immortal_before}, after={immortal_after}"
        )

    print("CASE_RESULT refcount_bit_branch OK")
    cinderx.jit.disassemble(_refcount_target)


class RefcountBitBranchTests(unittest.TestCase):
    @unittest.skipUnless(IS_AARCH64, "AArch64-specific refcount code shape")
    def test_possible_immortal_decref_uses_tbnz(self) -> None:
        env = os.environ.copy()
        for key in (
            "CINDERX_DISABLE",
            "CINDERX_JIT_DISABLE",
            "PYTHONJITDEBUG",
            "PYTHONJITDISABLE",
            "PYTHONJITDUMPASM",
        ):
            env.pop(key, None)
        env.update(
            {
                "CINDERX_PLUGIN_ENABLE": "1",
                "PYTHONUNBUFFERED": "1",
            }
        )
        completed = subprocess.run(
            [sys.executable, str(HELPER), "--jit-case"],
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        output = completed.stdout + completed.stderr
        self.assertEqual(completed.returncode, 0, output)
        self.assertIn("CASE_RESULT refcount_bit_branch OK", output)
        self.assertRegex(output.lower(), r"\btbnz\s+[wx]\d+,\s*#(?:0x)?1f\b")
        self.assertIsNone(
            re.search(
                r"\btst\s+w(\d+),\s*w\1.*\n\s*b\.(?:mi|pl)\b",
                output.lower(),
            ),
            output,
        )


if __name__ == "__main__":
    if "--jit-case" in sys.argv:
        _run_jit_case()
    else:
        unittest.main()
