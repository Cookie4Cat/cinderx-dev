# Copyright (c) Meta Platforms, Inc. and affiliates.

import math
import unittest
from typing import Any, Callable, TypeVar

import cinderx
import cinderx.jit
from cinderx.test_support import passIf

TCallableRet = TypeVar("TCallableRet")


def specialize(
    func: Callable[..., TCallableRet], callable: Callable[[], TCallableRet]
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)

    for _ in range(20):
        callable()

    cinderx.jit.jit_unsuppress(func)
    cinderx.jit.force_compile(func)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class KunpengPow2OptimizationTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_int_pow2_returns_int(self) -> None:
        def f(x: Any) -> Any:
            return x ** 2

        specialize(f, lambda: f(5))

        self.assertTrue(cinderx.jit.is_jit_compiled(f))

        result = f(5)
        self.assertEqual(result, 25)
        self.assertIs(type(result), int)

    def test_nan_pow2_matches_mul(self) -> None:
        def pow2(x: Any) -> Any:
            return x ** 2

        def mul(x: Any) -> Any:
            return x * x

        specialize(pow2, lambda: pow2(1.0))

        self.assertTrue(cinderx.jit.is_jit_compiled(pow2))

        nan = float("nan")
        pow_result = pow2(nan)
        mul_result = mul(nan)

        self.assertTrue(math.isnan(pow_result))
        self.assertTrue(math.isnan(mul_result))

    def test_neg_zero_pow2_returns_pos_zero(self) -> None:
        def f(x: Any) -> Any:
            return x ** 2

        specialize(f, lambda: f(1.0))

        self.assertTrue(cinderx.jit.is_jit_compiled(f))

        result = f(-0.0)
        self.assertEqual(result, 0.0)
        self.assertFalse(math.copysign(1.0, result) < 0.0)

    def test_int_pow2_deopt_count(self) -> None:
        def f(x: Any) -> Any:
            return x ** 2

        specialize(f, lambda: f(5))

        self.assertTrue(cinderx.jit.is_jit_compiled(f))

        cinderx.jit.clear_runtime_stats()

        f(5)
        stats = cinderx.jit.get_and_clear_runtime_stats()
        deopt_count = len(stats.get("deopt", []))
        self.assertLessEqual(deopt_count, 1)

    def test_pow2_overflow_error(self) -> None:
        def f(x: Any) -> Any:
            return x ** 2

        specialize(f, lambda: f(1.0))

        self.assertTrue(cinderx.jit.is_jit_compiled(f))

        with self.assertRaises(OverflowError):
            f(1e308)