# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import re
from pathlib import Path
import sys
import sysconfig
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import (
    assert_python_child_ok,
    passIf,
    run_python_child,
)


CHILD = Path(__file__).with_name("child_cases") / "load_attr_lir_fastpath.py"


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class LoadAttrLIRFastPathTests(unittest.TestCase):
    def test_load_attr_cached_lowers_to_aarch64_lir_fastpath(self) -> None:
        if sys.version_info < (3, 14):
            self.skipTest("LoadAttrCachedFastPath requires Python 3.14+")
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("LoadAttrCachedFastPath is disabled without the GIL")

        env = dict(os.environ)
        env["PYTHONJITDUMPLIR"] = "1"
        proc = run_python_child(CHILD, "cached-fastpath", env=env)
        dump = assert_python_child_ok(
            proc,
            context="LoadAttrCached AArch64 LIR fast path",
        )
        match = re.search(
            r"LIR for __main__:read after generation:\n"
            r"(.*?)(?:\nJIT: .*?LIR for |\Z)",
            dump,
            re.S,
        )
        self.assertIsNotNone(match, dump)
        self.assertIn("LoadAttrCachedFastPath", match.group(1), dump)

    def test_managed_dict_without_inline_values_falls_back_safely(self) -> None:
        if sys.version_info < (3, 14):
            self.skipTest("split inline values fast path requires Python 3.14+")
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("LoadAttrCachedFastPath is disabled without the GIL")

        proc = run_python_child(
            CHILD,
            "managed-dict-without-inline-values",
        )
        assert_python_child_ok(
            proc,
            context="managed dict without inline values fallback",
        )


if __name__ == "__main__":
    unittest.main()
