# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Regression test for https://gitcode.com/openeuler/cinderx/issues/16.

BuiltinLoadMethodElimination resolves method loads on exact immutable
receiver types at compile time. The MRO lookup legitimately comes back
empty when the method does not exist (or when a base type is not covered
by the builtin-members cache), and that empty result used to be fed
straight into Py_TYPE(), crashing the whole process inside
force_compile. A lookup miss must instead leave the LoadMethod in place
so the compiled code raises AttributeError at runtime, matching the
interpreter.
"""

import re
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf

_pattern = re.compile("x")


def call_missing_method():
    return _pattern.method_that_does_not_exist()


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class BuiltinLoadMethodElimMissingMethodTests(unittest.TestCase):
    def test_compiling_missing_method_call_keeps_attribute_error(self) -> None:
        cinderx.jit.force_uncompile(call_missing_method)
        cinderx.jit.jit_suppress(call_missing_method)
        try:
            # Populate type feedback for the re.Pattern receiver while the
            # missing method keeps raising in the interpreter.
            for _ in range(64):
                with self.assertRaises(AttributeError):
                    call_missing_method()
        finally:
            cinderx.jit.jit_unsuppress(call_missing_method)

        # This used to segfault inside BuiltinLoadMethodElimination
        # (Py_TYPE on a null method lookup result).
        self.assertTrue(cinderx.jit.force_compile(call_missing_method))
        self.assertTrue(cinderx.jit.is_jit_compiled(call_missing_method))

        # The compiled call must keep interpreter semantics: a catchable
        # AttributeError, not a crash and not a successful call.
        with self.assertRaises(AttributeError):
            call_missing_method()


if __name__ == "__main__":
    unittest.main()
