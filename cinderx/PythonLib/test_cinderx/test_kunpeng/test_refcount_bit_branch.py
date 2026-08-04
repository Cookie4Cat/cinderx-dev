import os
import platform
import re
import unittest
from pathlib import Path

import cinderx.jit
from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "refcount_bit_branch.py"
IS_AARCH64 = platform.machine().lower() in {"aarch64", "arm64"}


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
        completed = run_python_child(
            HELPER,
            env=env,
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
    unittest.main()
