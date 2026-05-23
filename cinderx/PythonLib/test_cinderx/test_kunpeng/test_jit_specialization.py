# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from typing import Any

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf
from test_cinderx.test_jit_specialization import opnames, specialize


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class KunpengSpecializationTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_binary_op_add_int_leaf_mixed_numbers_does_not_deopt(self) -> None:
        def f(a: Any, b: Any) -> Any:
            return a + b

        specialize(f, lambda: f(1, 2))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_ADD_INT", opnames(f))

        # fail_if_deopt wraps the already-specialized function and only watches
        # runtime stats; the wrapper itself does not participate in warmup.
        checked = fail_if_deopt(f)
        self.assertEqual(checked(1.25, 2.5), 3.75)

    def test_binary_op_add_int_loop_deopts_on_float(self) -> None:
        def f(xs: list[Any], init: Any) -> Any:
            s = init
            for x in xs:
                s = s + x
            return s

        specialize(f, lambda: f([1, 2, 3], 0))

        self.assertIn("BINARY_OP_ADD_INT", opnames(f))

        checked = fail_if_deopt(f)
        with self.assertRaisesRegex(RuntimeError, "Deopt occurred"):
            checked([1.25, 2.5], 0.0)

    def test_compare_op_int_leaf_mixed_numbers_does_not_deopt(self) -> None:
        def f(a: Any, b: Any) -> bool:
            return a < b

        specialize(f, lambda: f(1, 2))

        self.assertNotIn("COMPARE_OP", opnames(f))
        self.assertIn("COMPARE_OP_INT", opnames(f))

        # fail_if_deopt wraps the already-specialized function and only watches
        # runtime stats; the wrapper itself does not participate in warmup.
        checked = fail_if_deopt(f)
        self.assertEqual(checked(1.25, 2.5), True)

    def test_compare_op_int_loop_deopts_on_float(self) -> None:
        def f(xs: list[Any], limit: Any) -> int:
            count = 0
            for x in xs:
                if x < limit:
                    count += 1
            return count

        specialize(f, lambda: f([1, 2, 3], 4))

        self.assertIn("COMPARE_OP_INT", opnames(f))

        checked = fail_if_deopt(f)
        with self.assertRaisesRegex(RuntimeError, "Deopt occurred"):
            checked([1.25, 2.5], 4.0)


if __name__ == "__main__":
    unittest.main()
