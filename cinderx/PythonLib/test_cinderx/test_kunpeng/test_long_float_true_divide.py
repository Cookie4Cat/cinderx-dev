# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from typing import Any, Callable, TypeVar

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf


TCallableRet = TypeVar("TCallableRet")


def _specialize(
    func: Callable[..., TCallableRet],
    callable: Callable[[], TCallableRet],
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(20):
            callable()
    finally:
        cinderx.jit.jit_unsuppress(func)
    assert cinderx.jit.force_compile(func)
    assert cinderx.jit.is_jit_compiled(func)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class LongFloatTrueDivideTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_large_int_float_true_divide_does_not_deopt(self) -> None:
        def elapsed_seconds(elapsed_ns: Any) -> Any:
            return elapsed_ns / 1e9

        _specialize(elapsed_seconds, lambda: elapsed_seconds(1_234_567_890))

        checked = fail_if_deopt(elapsed_seconds)
        for elapsed_ns in (10**12, 10**20, -(10**20)):
            self.assertEqual(checked(elapsed_ns), elapsed_ns / 1e9)

    def test_zero_division_is_caught_in_same_frame(self) -> None:
        def divide_or_zero(value: Any, divisor: Any) -> Any:
            try:
                return value / divisor
            except ZeroDivisionError:
                return "zero"

        _specialize(divide_or_zero, lambda: divide_or_zero(5, 2.0))

        self.assertEqual(divide_or_zero(5, 0.0), "zero")
        self.assertEqual(divide_or_zero(5, -0.0), "zero")

    def test_zero_division_message_matches_python(self) -> None:
        def divide(value: Any, divisor: Any) -> Any:
            return value / divisor

        _specialize(divide, lambda: divide(5, 2.0))

        with self.assertRaisesRegex(ZeroDivisionError, "division by zero"):
            divide(5, 0.0)

    def test_overflow_is_caught_in_same_frame(self) -> None:
        def divide_or_overflow(value: Any, divisor: Any) -> Any:
            try:
                return value / divisor
            except OverflowError:
                return "overflow"

        _specialize(divide_or_overflow, lambda: divide_or_overflow(5, 2.0))

        self.assertEqual(divide_or_overflow(10**400, 1.0), "overflow")

    def test_overflow_precedes_zero_division_in_same_frame(self) -> None:
        def divide_or_error(value: Any, divisor: Any) -> Any:
            try:
                return value / divisor
            except OverflowError:
                return "overflow"
            except ZeroDivisionError:
                return "zero"

        _specialize(divide_or_error, lambda: divide_or_error(5, 2.0))

        self.assertEqual(divide_or_error(10**400, 0.0), "overflow")

    def test_float_int_compact_true_divide_does_not_deopt(self) -> None:
        def divide_by_int(divisor: Any) -> Any:
            return 1.5 / divisor

        _specialize(divide_by_int, lambda: divide_by_int(3))

        checked = fail_if_deopt(divide_by_int)
        for divisor in (1, 2, 7):
            self.assertEqual(checked(divisor), 1.5 / divisor)

    def test_float_int_noncompact_true_divide_matches_python(
        self,
    ) -> None:
        def divide_by_int(divisor: Any) -> Any:
            return 1.5 / divisor

        _specialize(divide_by_int, lambda: divide_by_int(3))

        divisor = 10**20
        self.assertEqual(divide_by_int(divisor), 1.5 / divisor)


if __name__ == "__main__":
    unittest.main()
