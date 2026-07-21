# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import math
import unittest
from typing import Any, Callable

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf
from test_cinderx.test_jit_specialization import specialize


PowerFunc = Callable[[Any], Any]


def _pow_p05(x: Any) -> Any:
    return (x * 1.0) ** 0.5


def _pow_p10(x: Any) -> Any:
    return (x * 1.0) ** 1.0


def _pow_p15(x: Any) -> Any:
    return (x * 1.0) ** 1.5


def _pow_p20(x: Any) -> Any:
    return (x * 1.0) ** 2.0


def _pow_p30(x: Any) -> Any:
    return (x * 1.0) ** 3.0


def _pow_n05(x: Any) -> Any:
    return (x * 1.0) ** -0.5


def _pow_n10(x: Any) -> Any:
    return (x * 1.0) ** -1.0


def _pow_n15(x: Any) -> Any:
    return (x * 1.0) ** -1.5


def _pow_n20(x: Any) -> Any:
    return (x * 1.0) ** -2.0


POWER_CASES: tuple[tuple[PowerFunc, float, tuple[float, ...], int], ...] = (
    (_pow_p05, 0.5, (4.0, 16.0), 2),
    (_pow_p10, 1.0, (-4.0, 2.0), 1),
    (_pow_p15, 1.5, (4.0, 16.0), 3),
    (_pow_p20, 2.0, (-4.0, 2.0), 2),
    (_pow_p30, 3.0, (-4.0, 2.0), 3),
    (_pow_n05, -0.5, (4.0, 16.0), 3),
    (_pow_n10, -1.0, (-4.0, 2.0), 2),
    (_pow_n15, -1.5, (4.0, 16.0), 4),
    (_pow_n20, -2.0, (-4.0, 2.0), 3),
)

FRACTIONAL_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_p05, 0.5),
    (_pow_p15, 1.5),
    (_pow_n05, -0.5),
    (_pow_n15, -1.5),
)

NEGATIVE_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_n05, -0.5),
    (_pow_n10, -1.0),
    (_pow_n15, -1.5),
    (_pow_n20, -2.0),
)

POSITIVE_CASES: tuple[tuple[PowerFunc, float], ...] = (
    (_pow_p05, 0.5),
    (_pow_p10, 1.0),
    (_pow_p15, 1.5),
    (_pow_p20, 2.0),
    (_pow_p30, 3.0),
)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class FloatPowerStrengthReductionTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def _specialize(self, func: PowerFunc) -> None:
        specialize(func, lambda: func(4.0))
        self.assertTrue(cinderx.jit.is_jit_compiled(func))

    def test_rewritten_exponents_use_expected_hir_and_do_not_deopt(
        self,
    ) -> None:
        # Counts include the leading x * 1.0 that makes the base FloatExact.
        for func, exponent, values, expected_double_ops in POWER_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)

                counts = cinderx.jit.get_function_hir_opcode_counts(func)
                self.assertIsNotNone(counts)
                assert counts is not None
                self.assertEqual(
                    counts.get("DoubleBinaryOp", 0),
                    expected_double_ops,
                    dict(counts),
                )
                self.assertEqual(counts.get("FloatBinaryOp", 0), 0, dict(counts))
                self.assertEqual(counts.get("BinaryOp", 0), 0, dict(counts))

                checked = fail_if_deopt(func)
                for value in values:
                    self.assertEqual(checked(value), value**exponent)

    def test_negative_fractional_base_falls_back_to_complex_power(self) -> None:
        for func, exponent in FRACTIONAL_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                result = func(-4.0)
                self.assertIs(type(result), complex)
                self.assertEqual(result, (-4.0) ** exponent)

    def test_signed_zero_negative_exponents_raise(self) -> None:
        for func, exponent in NEGATIVE_CASES:
            for value in (0.0, -0.0):
                with self.subTest(exponent=exponent, value=value):
                    self._specialize(func)
                    with self.assertRaises(ZeroDivisionError):
                        func(value)

    def test_negative_zero_positive_exponents_preserve_python_sign(self) -> None:
        for func, exponent in POSITIVE_CASES:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                result = func(-0.0)
                expected = (-0.0) ** exponent
                self.assertEqual(result, expected)
                self.assertEqual(
                    math.copysign(1.0, result), math.copysign(1.0, expected)
                )

    def test_result_overflow_raises(self) -> None:
        cases: tuple[tuple[PowerFunc, float], ...] = (
            (_pow_p15, 1e250),
            (_pow_p20, 1e200),
            (_pow_p30, -1e200),
            (_pow_n10, 1e-320),
            (_pow_n10, -1e-320),
            (_pow_n15, 1e-250),
            (_pow_n20, 1e-200),
        )
        for func, value in cases:
            with self.subTest(func=func.__name__, value=value):
                self._specialize(func)
                with self.assertRaises(OverflowError):
                    func(value)

    def test_intermediate_overflow_falls_back_to_subnormal_result(self) -> None:
        cases: tuple[tuple[PowerFunc, float, float], ...] = (
            (_pow_n15, 1e210, -1.5),
            (_pow_n20, 1e160, -2.0),
        )
        for func, value, exponent in cases:
            with self.subTest(exponent=exponent):
                self._specialize(func)
                expected = value**exponent
                self.assertNotEqual(expected, 0.0)
                self.assertEqual(func(value), expected)

    def test_nan_and_infinities_match_python(self) -> None:
        nan = float("nan")
        infinities = (float("inf"), float("-inf"))

        for func, exponent, _, _ in POWER_CASES:
            with self.subTest(exponent=exponent, value="nan"):
                self._specialize(func)
                self.assertTrue(math.isnan(func(nan)))

            for value in infinities:
                with self.subTest(exponent=exponent, value=value):
                    self._specialize(func)
                    result = func(value)
                    expected = value**exponent
                    self.assertEqual(result, expected)
                    if expected == 0.0 or math.isinf(expected):
                        self.assertEqual(
                            math.copysign(1.0, result),
                            math.copysign(1.0, expected),
                        )


if __name__ == "__main__":
    unittest.main()
