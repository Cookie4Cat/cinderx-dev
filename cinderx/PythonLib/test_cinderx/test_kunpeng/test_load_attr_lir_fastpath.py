# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import re
import subprocess
import sys
import sysconfig
import tempfile
import textwrap
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class LoadAttrLIRFastPathTests(unittest.TestCase):
    def test_load_attr_cached_lowers_to_aarch64_lir_fastpath(self) -> None:
        if sys.version_info < (3, 14):
            self.skipTest("LoadAttrCachedFastPath requires Python 3.14+")
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("LoadAttrCachedFastPath is disabled without the GIL")

        code = textwrap.dedent(
            """
            import cinderx.jit as jit

            jit.enable()
            jit.enable_specialized_opcodes()
            jit.compile_after_n_calls(1000000)

            class Box:
                pass

            class Other:
                pass

            class Descriptor:
                def __get__(self, obj, typ=None):
                    if obj is None:
                        return self
                    return obj.seed + 5

            class WithDescriptor:
                value = Descriptor()

            class SlotBox:
                __slots__ = ("value",)

            box = Box()
            box.value = 42

            def read(obj):
                return obj.value

            for _ in range(20000):
                read(box)

            assert jit.force_compile(read)
            counts = jit.get_function_hir_opcode_counts(read)
            print(counts.get("LoadAttrCached", 0))
            print(read(box))

            other = Other()
            other.value = 43
            assert read(other) == 43

            slot = SlotBox()
            slot.value = 44
            assert read(slot) == 44

            descr = WithDescriptor()
            descr.seed = 45
            assert read(descr) == 50

            try:
                read(Other())
            except AttributeError:
                pass
            else:
                raise AssertionError("missing attribute should raise AttributeError")

            def custom_getattribute(self, name):
                if name == "value":
                    return 99
                return object.__getattribute__(self, name)

            Box.__getattribute__ = custom_getattribute
            assert read(box) == 99
            """
        )

        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "load_attr_lir_fastpath.py")
            with open(script, "w", encoding="utf-8") as fp:
                fp.write(code)

            env = dict(os.environ)
            env["PYTHONJITDUMPLIR"] = "1"
            proc = subprocess.run(
                [sys.executable, script],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
            )
            self.assertEqual(
                proc.returncode,
                0,
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
            )

        dump = proc.stdout + "\n" + proc.stderr
        match = re.search(
            r"LIR for __main__:read after generation:\n"
            r"(.*?)(?:\nJIT: .*?LIR for |\Z)",
            dump,
            re.S,
        )
        self.assertIsNotNone(match, dump)
        self.assertIn("LoadAttrCachedFastPath", match.group(1), dump)

        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        self.assertGreaterEqual(len(lines), 2, proc.stdout)
        self.assertGreaterEqual(int(lines[-2]), 1, proc.stdout)
        self.assertEqual(int(lines[-1]), 42, proc.stdout)

    def test_managed_dict_without_inline_values_falls_back_safely(self) -> None:
        if sys.version_info < (3, 14):
            self.skipTest("split inline values fast path requires Python 3.14+")
        if sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("LoadAttrCachedFastPath is disabled without the GIL")

        code = textwrap.dedent(
            """
            import cinderx.jit as jit

            jit.enable()
            jit.enable_specialized_opcodes()
            jit.compile_after_n_calls(1000000)

            class EncodingBytes(bytes):
                def __new__(cls, value):
                    return bytes.__new__(cls, value)

                def __init__(self, value):
                    self._position = 0

                def match_bytes(self, needle):
                    return self.startswith(needle, self._position)

            buf = EncodingBytes(b"abcdef")
            assert jit.force_compile(EncodingBytes.match_bytes)
            assert buf.match_bytes(b"abc") is True
            """
        )

        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "managed_dict_without_inline_values.py")
            with open(script, "w", encoding="utf-8") as fp:
                fp.write(code)

            proc = subprocess.run(
                [sys.executable, script],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(
                proc.returncode,
                0,
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
