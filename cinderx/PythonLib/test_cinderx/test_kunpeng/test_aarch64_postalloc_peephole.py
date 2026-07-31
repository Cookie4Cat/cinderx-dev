# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import platform
import unittest

import cinderx.jit
from cinderx.test_support import skip_unless_jit


@unittest.skipUnless(
    platform.machine().lower() in {"aarch64", "arm64"},
    "Tests the AArch64 JIT backend",
)
@skip_unless_jit("Tests AArch64 post-register-allocation peepholes")
class AArch64PostAllocPeepholeIntegrationTest(unittest.TestCase):
    def test_adjacent_object_spills_preserve_identity_and_order(self) -> None:
        def target(
            reverse: bool,
            first: object,
            second: object,
            third: object,
            fourth: object,
        ) -> tuple[object, object, object, object]:
            # Keep several object values live together so the JIT has realistic
            # adjacent spill/fill opportunities.  Machine-code path-hit is
            # checked separately; this test protects observable Python
            # identity and ordering across the optimized code.
            values = (first, second, third, fourth)
            if reverse:
                return values[3], values[2], values[1], values[0]
            return values

        if not cinderx.jit.is_jit_compiled(target):
            self.assertTrue(cinderx.jit.force_compile(target))
        self.assertTrue(cinderx.jit.is_jit_compiled(target))

        values = tuple(object() for _ in range(4))
        self.assertEqual(target(False, *values), values)
        self.assertEqual(target(True, *values), tuple(reversed(values)))

    def test_multiply_accumulate_and_indexing_preserve_python_semantics(
        self,
    ) -> None:
        def multiply_add(a: int, b: int, accumulator: int) -> int:
            return a * b + accumulator

        def multiply_subtract(a: int, b: int, accumulator: int) -> int:
            return accumulator - a * b

        def indexed_byte(data: bytes, index: int) -> int:
            return data[index]

        for target in (multiply_add, multiply_subtract, indexed_byte):
            if not cinderx.jit.is_jit_compiled(target):
                self.assertTrue(cinderx.jit.force_compile(target))
            self.assertTrue(cinderx.jit.is_jit_compiled(target))

        arithmetic_cases = (
            (7, 9, 11),
            (-7, 9, 11),
            (0, 0, 0),
            ((1 << 62) - 1, 3, -5),
        )
        for a, b, accumulator in arithmetic_cases:
            self.assertEqual(
                multiply_add(a, b, accumulator),
                a * b + accumulator,
            )
            self.assertEqual(
                multiply_subtract(a, b, accumulator),
                accumulator - a * b,
            )

        data = bytes(range(128))
        for index in (0, 1, 31, 63, 64, 127, -1):
            self.assertEqual(indexed_byte(data, index), data[index])


if __name__ == "__main__":
    unittest.main()
