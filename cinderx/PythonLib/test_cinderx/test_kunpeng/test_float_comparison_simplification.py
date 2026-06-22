# Copyright (c) Meta Platforms, Inc. and affiliates.

import math
import unittest
from typing import Any, Callable, TypeVar

import cinderx
import cinderx.jit


TCallableRet = TypeVar("TCallableRet")


def _specialize(func: Callable[..., TCallableRet], *warmup_args: Any) -> None:
    """Trigger adaptive specialization and JIT-compile the function."""
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(20):
            func(*warmup_args)
    finally:
        cinderx.jit.jit_unsuppress(func)
    cinderx.jit.force_compile(func)
    assert cinderx.jit.is_jit_compiled(func), f"{func.__name__} was not compiled"


@unittest.skipIf(
    not cinderx.jit.is_enabled(), "Tests functionality on the JIT"
)
class FloatComparisonSimplificationTests(unittest.TestCase):
    """End-to-end tests for the FloatComparisonSimplification HIR pass.

    Verifies that float comparisons produce results identical to the
    interpreter, including edge cases like NaN, infinities, and signed
    zero.  The HIR-level pattern matching is covered by the HIR text
    fixtures (float_comparison_simplification_test.txt).
    """

    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    # ------------------------------------------------------------------
    # S8 / E8:  Float comparison directly drives a conditional branch.
    # ------------------------------------------------------------------

    def test_greater_than_branch_true(self) -> None:
        """S8: a > b is True drives the then-branch."""
        def gt_branch(a: float, b: float) -> int:
            if a > b:
                return 1
            return 0

        _specialize(gt_branch, 2.0, 1.0)

        self.assertEqual(gt_branch(2.0, 1.0), 1)
        self.assertEqual(gt_branch(1.0, 2.0), 0)
        self.assertEqual(gt_branch(1.0, 1.0), 0)

    def test_less_than_branch_true(self) -> None:
        """S8: a < b is True drives the then-branch."""
        def lt_branch(a: float, b: float) -> int:
            if a < b:
                return 42
            return -1

        _specialize(lt_branch, 1.0, 2.0)

        self.assertEqual(lt_branch(1.0, 2.0), 42)
        self.assertEqual(lt_branch(2.0, 1.0), -1)

    def test_greater_equal_branch_true(self) -> None:
        """S8: a >= b with both > and == cases."""
        def ge_branch(a: float, b: float) -> int:
            if a >= b:
                return 1
            return 0

        _specialize(ge_branch, 2.0, 1.0)

        self.assertEqual(ge_branch(2.0, 1.0), 1)
        self.assertEqual(ge_branch(1.0, 2.0), 0)
        self.assertEqual(ge_branch(1.0, 1.0), 1)

    def test_less_equal_branch_true(self) -> None:
        """S8: a <= b with both < and == cases."""
        def le_branch(a: float, b: float) -> int:
            if a <= b:
                return 1
            return 0

        _specialize(le_branch, 1.0, 2.0)

        self.assertEqual(le_branch(1.0, 2.0), 1)
        self.assertEqual(le_branch(2.0, 1.0), 0)
        self.assertEqual(le_branch(1.0, 1.0), 1)

    def test_equal_branch_true(self) -> None:
        """S8: a == b drives the branch."""
        def eq_branch(a: float, b: float) -> int:
            if a == b:
                return 1
            return 0

        _specialize(eq_branch, 1.0, 1.0)

        self.assertEqual(eq_branch(1.0, 1.0), 1)
        self.assertEqual(eq_branch(1.0, 2.0), 0)

    def test_not_equal_branch_true(self) -> None:
        """S8: a != b drives the branch."""
        def ne_branch(a: float, b: float) -> int:
            if a != b:
                return 1
            return 0

        _specialize(ne_branch, 1.0, 2.0)

        self.assertEqual(ne_branch(1.0, 2.0), 1)
        self.assertEqual(ne_branch(1.0, 1.0), 0)

    # ------------------------------------------------------------------
    # S9 / E9:  NaN unordered comparison preserves Python semantics.
    # ------------------------------------------------------------------

    def test_nan_greater_than_always_false(self) -> None:
        """S9: NaN > x and x > NaN both return False (IEEE 754)."""
        def gt_branch(a: float, b: float) -> int:
            if a > b:
                return 1
            return 0

        _specialize(gt_branch, 1.0, 1.0)
        nan = float("nan")

        self.assertEqual(gt_branch(nan, 1.0), 0, "NaN > 1.0 should be False")
        self.assertEqual(gt_branch(1.0, nan), 0, "1.0 > NaN should be False")
        self.assertEqual(gt_branch(nan, nan), 0, "NaN > NaN should be False")

    def test_nan_less_than_always_false(self) -> None:
        """S9: NaN < x and x < NaN both return False."""
        def lt_branch(a: float, b: float) -> int:
            if a < b:
                return 1
            return 0

        _specialize(lt_branch, 1.0, 2.0)
        nan = float("nan")

        self.assertEqual(lt_branch(nan, 1.0), 0, "NaN < 1.0 should be False")
        self.assertEqual(lt_branch(1.0, nan), 0, "1.0 < NaN should be False")

    def test_nan_not_equal_always_true(self) -> None:
        """S9: NaN != x returns True for any x (including NaN)."""
        def ne_branch(a: float, b: float) -> int:
            if a != b:
                return 1
            return 0

        _specialize(ne_branch, 1.0, 2.0)
        nan = float("nan")

        self.assertEqual(ne_branch(nan, 1.0), 1, "NaN != 1.0 should be True")
        self.assertEqual(ne_branch(1.0, nan), 1, "1.0 != NaN should be True")
        self.assertEqual(ne_branch(nan, nan), 1, "NaN != NaN should be True")

    def test_nan_equal_always_false(self) -> None:
        """S9: NaN == x returns False for any x (including NaN)."""
        def eq_branch(a: float, b: float) -> int:
            if a == b:
                return 1
            return 0

        _specialize(eq_branch, 1.0, 1.0)
        nan = float("nan")

        self.assertEqual(eq_branch(nan, 1.0), 0, "NaN == 1.0 should be False")
        self.assertEqual(eq_branch(nan, nan), 0, "NaN == NaN should be False")

    def test_nan_greater_equal_always_false(self) -> None:
        """S9: NaN >= x returns False."""
        def ge_branch(a: float, b: float) -> int:
            if a >= b:
                return 1
            return 0

        _specialize(ge_branch, 1.0, 1.0)
        nan = float("nan")

        self.assertEqual(ge_branch(nan, 1.0), 0, "NaN >= 1.0 should be False")
        self.assertEqual(ge_branch(1.0, nan), 0, "1.0 >= NaN should be False")

    def test_nan_less_equal_always_false(self) -> None:
        """S9: NaN <= x returns False."""
        def le_branch(a: float, b: float) -> int:
            if a <= b:
                return 1
            return 0

        _specialize(le_branch, 1.0, 2.0)
        nan = float("nan")

        self.assertEqual(le_branch(nan, 1.0), 0, "NaN <= 1.0 should be False")
        self.assertEqual(le_branch(1.0, nan), 0, "1.0 <= NaN should be False")

    def test_inf_comparison(self) -> None:
        """S9: +inf / -inf comparisons are correct."""
        def gt_branch(a: float, b: float) -> int:
            if a > b:
                return 1
            return 0

        _specialize(gt_branch, 1.0, 1.0)
        inf = float("inf")
        neg_inf = float("-inf")

        self.assertEqual(gt_branch(inf, 1.0), 1, "+inf > 1.0 should be True")
        self.assertEqual(gt_branch(1.0, inf), 0, "1.0 > +inf should be False")
        self.assertEqual(gt_branch(neg_inf, 1.0), 0, "-inf > 1.0 should be False")
        self.assertEqual(gt_branch(1.0, neg_inf), 1, "1.0 > -inf should be True")

    def test_neg_zero_comparison(self) -> None:
        """S9: -0.0 == 0.0 in Python (IEEE 754 equality)."""
        def eq_branch(a: float, b: float) -> int:
            if a == b:
                return 1
            return 0

        _specialize(eq_branch, 1.0, 1.0)

        self.assertEqual(eq_branch(-0.0, 0.0), 1, "-0.0 == 0.0 should be True")
        self.assertEqual(eq_branch(0.0, -0.0), 1, "0.0 == -0.0 should be True")

    # ------------------------------------------------------------------
    # S10 / E10:  Comparison result returned as Python object.
    # ------------------------------------------------------------------

    def test_comparison_result_is_bool_object(self) -> None:
        """S10: Returning a comparison result preserves True/False identity."""
        def cmp_result(a: float, b: float) -> Any:
            return a > b

        _specialize(cmp_result, 1.0, 1.0)

        self.assertIs(cmp_result(2.0, 1.0), True)
        self.assertIs(cmp_result(1.0, 2.0), False)

    def test_comparison_result_twice_yields_same_object(self) -> None:
        """S10: Multiple calls return the same True/False singletons."""
        def cmp_result(a: float, b: float) -> Any:
            return a >= b

        _specialize(cmp_result, 2.0, 1.0)

        r1 = cmp_result(2.0, 1.0)
        r2 = cmp_result(2.0, 1.0)
        self.assertIs(r1, r2)
        self.assertIs(r1, True)


if __name__ == "__main__":
    unittest.main()
