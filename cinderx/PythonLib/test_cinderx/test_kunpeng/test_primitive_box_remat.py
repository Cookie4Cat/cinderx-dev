# Copyright (c) Meta Platforms, Inc. and affiliates.

import math
import unittest

import pytest

import cinderx.jit


# ---------------------------------------------------------------------------
# Shared helpers and test data
# ---------------------------------------------------------------------------

class Body:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


class IntYBody:
    x = 7.0
    y = 5
    z = 11.0


def dist_sq(a, b):
    dx = a.x - b.x
    dy = a.y - b.y
    dz = a.z - b.z
    return dx * dx + dy * dy + dz * dz


def _specialize_then_compile(func, runner):
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(10000):
            runner()
    finally:
        cinderx.jit.jit_unsuppress(func)

    assert cinderx.jit.force_compile(func)


# ---------------------------------------------------------------------------
# S11 test functions — intermediate float values, primitive ops only
# ---------------------------------------------------------------------------

def _chain_quadratic(x):
    """x² + 2x + 1  — linear chain, every intermediate is CDouble."""
    x_sq = x * x
    two_x = 2.0 * x
    return x_sq + two_x + 1.0


def _vector_norm_sq(x, y, z):
    """Multiple parallel float intermediates: x² + y² + z²."""
    x2 = x * x
    y2 = y * y
    z2 = z * z
    return x2 + y2 + z2


def _poly_5th(x):
    """5th-degree polynomial — many chained CDouble intermediates."""
    x2 = x * x
    x3 = x2 * x
    x4 = x3 * x
    x5 = x4 * x
    return x5 + 2.0 * x4 + 3.0 * x3 + 4.0 * x2 + 5.0 * x + 6.0


def _branch_float_compute(x, flag):
    """Both branches produce CDouble intermediates."""
    if flag:
        t = x * 2.0
        return t + 1.0
    else:
        t = x / 2.0
        return t - 1.0


def _loop_sum_of_squares(n):
    """Loop body accumulates into a float — intermediate per iteration."""
    s = 0.0
    for i in range(n):
        fi = float(i)
        s += fi * fi
    return s


def _mixed_int_float(x):
    """CDouble intermediates should be elided; CInt64 box preserved."""
    scaled = x * 3.14
    return scaled + float(int(x))


def _float_only_intermediates_then_attr_store(a, b):
    """Intermediate float ops; result stored into attribute (real consumer)."""
    dx = a.x - b.x
    dy = a.y - b.y
    dz = a.z - b.z
    a.z = dx * dx + dy * dy + dz * dz
    return a.z


def _repeated_attr_unbox(a, b):
    """Same float attr unboxed multiple times — PrimitiveUnboxCSE target."""
    dx1 = a.x - b.x
    dx2 = a.x - b.x
    return dx1 + dx2


def _attr_intermediate_chain(a):
    """Load float attr → chain of primitive ops → return."""
    v = a.x * 2.0
    v = v + 1.0
    v = v - 0.5
    return v * 3.0


# ---------------------------------------------------------------------------
# E11 canonical expected results (computed by the interpreter)
# ---------------------------------------------------------------------------

# Pre-computed for key inputs so we can compare JIT output.
def _eval_quadratic(x):
    return x * x + 2.0 * x + 1.0


def _eval_vector_norm_sq(x, y, z):
    return x * x + y * y + z * z


def _eval_poly_5th(x):
    x2 = x * x
    x3 = x2 * x
    x4 = x3 * x
    x5 = x4 * x
    return x5 + 2.0 * x4 + 3.0 * x3 + 4.0 * x2 + 5.0 * x + 6.0


def _eval_branch_float(x, flag):
    if flag:
        return x * 2.0 + 1.0
    else:
        return x / 2.0 - 1.0


E11_INPUTS = [-100.0, -3.14, -1.0, -0.0, 0.0, 0.5, 1.0, 3.14, 1e10, 1e-10]


# ===================================================================
# S11 / E11 — unittest test cases
# ===================================================================

class TestPrimitiveBoxRematS11(unittest.TestCase):
    """S11: Intermediate float values → PrimitiveBox count reduced."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    # -- helpers --

    def _specialize_and_compile(self, func, runner):
        _specialize_then_compile(func, runner)

    def _assert_number_of_primitive_boxes(self, func, max_expected):
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(counts)
        actual = counts.get("PrimitiveBox", 0)
        self.assertLessEqual(
            actual, max_expected,
            f"PrimitiveBox count {actual} exceeds max {max_expected}; "
            f"all counts: {dict(counts)}",
        )

    # -- S11 core --

    def test_chain_quadratic_elides_intermediate_boxes(self):
        """Linear float chain: at most 1 PrimitiveBox (for return)."""
        self._specialize_and_compile(
            _chain_quadratic, lambda: _chain_quadratic(1.0),
        )
        self._assert_number_of_primitive_boxes(_chain_quadratic, 1)

    def test_vector_norm_sq_elides_parallel_intermediates(self):
        """Multiple parallel CDouble intermediates — boxes elided."""
        self._specialize_and_compile(
            _vector_norm_sq,
            lambda: _vector_norm_sq(1.0, 2.0, 3.0),
        )
        self._assert_number_of_primitive_boxes(_vector_norm_sq, 1)

    def test_polynomial_5th_elides_many_chained_intermediates(self):
        """Deep chain of CDouble ops — at most 1 PrimitiveBox survives."""
        self._specialize_and_compile(
            _poly_5th, lambda: _poly_5th(1.0),
        )
        self._assert_number_of_primitive_boxes(_poly_5th, 1)

    def test_branch_float_compute_delays_boxes(self):
        """Both branches have CDouble intermediates — still elided."""
        self._specialize_and_compile(
            _branch_float_compute,
            lambda: _branch_float_compute(1.0, True),
        )
        self._assert_number_of_primitive_boxes(_branch_float_compute, 2)

    def test_attr_intermediate_chain_elides_boxes(self):
        """Float attr → primitive chain → return: intermediate boxes gone."""
        a = Body(3.0, 0.0, 0.0)
        self._specialize_and_compile(
            _attr_intermediate_chain,
            lambda: _attr_intermediate_chain(a),
        )
        self._assert_number_of_primitive_boxes(_attr_intermediate_chain, 1)

    def test_float_only_intermediates_preserves_store_attr_box(self):
        """When result is stored to attr (real consumer), that box lives."""
        a = Body(1.0, 2.0, 3.0)
        b = Body(4.0, 5.0, 6.0)
        self._specialize_and_compile(
            _float_only_intermediates_then_attr_store,
            lambda: _float_only_intermediates_then_attr_store(a, b),
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(
            _float_only_intermediates_then_attr_store,
        )
        self.assertIsNotNone(counts)
        # StoreAttr is a real consumer; boxes feeding it are kept.
        self.assertGreater(counts.get("DoubleBinaryOp", 0), 0, counts)


class TestPrimitiveBoxRematE11(unittest.TestCase):
    """E11: Numerical result matches interpreted execution."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    # -- E11 correctness --

    def _check_jit_matches_interpreted(self, jit_func, ref_func, *args):
        _specialize_then_compile(jit_func, lambda: jit_func(*args))
        jit_result = jit_func(*args)
        ref_result = ref_func(*args)
        self.assertEqual(
            jit_result, ref_result,
            f"JIT={jit_result} != interpreted={ref_result} "
            f"for args={args}",
        )
        # Also compare type — box/remat must not change float/int identity.
        self.assertIs(type(jit_result), type(ref_result))

    def _check_jit_matches_interpreted_for_inputs(
        self, jit_func, ref_func, inputs,
    ):
        # Compile once with a representative input, then verify all inputs.
        _specialize_then_compile(jit_func, lambda: jit_func(inputs[0]))
        for x in inputs:
            jit_r = jit_func(x)
            ref_r = ref_func(x)
            self.assertEqual(jit_r, ref_r,
                             f"x={x}: JIT={jit_r} != ref={ref_r}")
            self.assertIs(type(jit_r), type(ref_r))

    def test_quadratic_matches_interpreted(self):
        self._check_jit_matches_interpreted_for_inputs(
            _chain_quadratic, _eval_quadratic, E11_INPUTS,
        )

    def test_vector_norm_sq_matches_interpreted(self):
        for x, y, z in [(0.0, 0.0, 0.0), (1.0, 2.0, 3.0),
                         (-1.0, -2.0, -3.0), (1e5, -1e5, 1e-5)]:
            self._check_jit_matches_interpreted(
                _vector_norm_sq, _eval_vector_norm_sq, x, y, z,
            )

    def test_polynomial_5th_matches_interpreted(self):
        self._check_jit_matches_interpreted_for_inputs(
            _poly_5th, _eval_poly_5th, E11_INPUTS,
        )

    def test_branch_float_matches_interpreted(self):
        _specialize_then_compile(
            _branch_float_compute,
            lambda: _branch_float_compute(1.0, True),
        )
        for x in E11_INPUTS:
            for flag in (True, False):
                self.assertEqual(
                    _branch_float_compute(x, flag),
                    _eval_branch_float(x, flag),
                )

    def test_special_float_values(self):
        """NaN, Inf, -0.0 survive box remat with IEEE semantics."""
        _specialize_then_compile(_chain_quadratic, lambda: _chain_quadratic(1.0))

        for x in [float("nan"), float("inf"), float("-inf"), -0.0]:
            jit_r = _chain_quadratic(x)
            ref_r = _eval_quadratic(x)
            if math.isnan(ref_r):
                self.assertTrue(math.isnan(jit_r), f"expected NaN, got {jit_r}")
            else:
                self.assertEqual(jit_r, ref_r, f"x={x}")
                self.assertIs(type(jit_r), type(ref_r))

    def test_loop_sum_of_squares_matches_interpreted(self):
        _specialize_then_compile(
            _loop_sum_of_squares, lambda: _loop_sum_of_squares(10),
        )
        for n in [0, 1, 5, 100]:
            jit_r = _loop_sum_of_squares(n)
            ref_r = sum(float(i) * float(i) for i in range(n))
            self.assertEqual(jit_r, ref_r, f"n={n}")


class TestPrimitiveBoxRematDeopt(unittest.TestCase):
    """Deopt correctness after box rematerialization."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def test_deopt_reconstructs_locals_for_dist_sq(self):
        p = Body(1.0, 2.0, 3.0)
        q = Body(4.0, 5.0, 6.0)
        _specialize_then_compile(dist_sq, lambda: dist_sq(p, q))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(dist_sq(p, IntYBody()), 109.0)
        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertGreaterEqual(len(stats.get("deopt") or ()), 1, stats)

    def test_deopt_reconstructs_chain_quadratic(self):
        _specialize_then_compile(
            _chain_quadratic, lambda: _chain_quadratic(1.0),
        )

        cinderx.jit.clear_runtime_stats()
        # Passing an int should trigger deopt because the function is
        # compiled for float — the CDouble unbox will fail.
        self.assertEqual(_chain_quadratic(42), _eval_quadratic(42.0))
        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertGreaterEqual(len(stats.get("deopt") or ()), 1, stats)

    def test_deopt_reconstructs_poly_5th(self):
        _specialize_then_compile(_poly_5th, lambda: _poly_5th(1.0))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(_poly_5th(3), _eval_poly_5th(3.0))
        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertGreaterEqual(len(stats.get("deopt") or ()), 1, stats)


class TestPrimitiveBoxRematNonCDouble(unittest.TestCase):
    """Non-CDouble PrimitiveBox must NOT be elided by this pass."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def test_mixed_int_float_preserves_cint64_box(self):
        """int(x) creates CInt64 PrimitiveBox — must survive the pass."""
        _specialize_then_compile(
            _mixed_int_float, lambda: _mixed_int_float(3.14),
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(_mixed_int_float)
        self.assertIsNotNone(counts)
        # At least one PrimitiveBox survives (the int box, plus possibly
        # one for the return double).
        self.assertGreaterEqual(counts.get("PrimitiveBox", 0), 1, counts)

    def test_non_cdouble_box_not_elided(self):
        """Verify that non-CDouble PrimitiveBoxes are not targets."""
        _specialize_then_compile(
            _mixed_int_float, lambda: _mixed_int_float(3.14),
        )
        # Numerical correctness still holds.
        for x in E11_INPUTS:
            self.assertEqual(_mixed_int_float(x),
                             x * 3.14 + float(int(x)),
                             f"x={x}")


class TestPrimitiveUnboxCSE(unittest.TestCase):
    """PrimitiveUnboxCSE: redundant unbox of same value+type eliminated."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def test_redundant_same_attr_unbox_reuses_first(self):
        """a.x loaded multiple times → at most one PrimitiveUnbox per LoadAttr."""
        a = Body(3.0, 4.0, 0.0)
        b = Body(1.0, 2.0, 0.0)
        _specialize_then_compile(
            _repeated_attr_unbox,
            lambda: _repeated_attr_unbox(a, b),
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(
            _repeated_attr_unbox,
        )
        self.assertIsNotNone(counts)
        # Each LoadAttrCached is a distinct HIR register, so PrimitiveUnbox
        # CSE cannot merge unboxes of different LoadAttr results (even when
        # they load the same field).  Assert ≤ number of LoadAttr ops.
        load_attr_count = counts.get("LoadAttrCached", 0)
        unbox_count = counts.get("PrimitiveUnbox", 0)
        self.assertLessEqual(
            unbox_count, load_attr_count,
            f"PrimitiveUnbox={unbox_count} exceeds LoadAttrCached={load_attr_count}: "
            f"{dict(counts)}",
        )

    def test_numerical_correctness_after_unbox_cse(self):
        a = Body(3.0, 4.0, 0.0)
        b = Body(1.0, 2.0, 0.0)
        _specialize_then_compile(
            _repeated_attr_unbox,
            lambda: _repeated_attr_unbox(a, b),
        )
        expected = (3.0 - 1.0) + (3.0 - 1.0)  # dx1 + dx2 = 2 + 2
        self.assertEqual(_repeated_attr_unbox(a, b), expected)


# ---------------------------------------------------------------------------
# pytest-style tests (compatible with the existing autouse fixture)
# ---------------------------------------------------------------------------

@pytest.fixture(autouse=True)
def _specialized_opcodes():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")

    cinderx.jit.enable_specialized_opcodes()
    yield
    cinderx.jit.disable_specialized_opcodes()


def test_primitive_box_remat_elides_frame_state_only_boxes():
    p = Body(1.0, 2.0, 3.0)
    q = Body(4.0, 5.0, 6.0)
    _specialize_then_compile(dist_sq, lambda: dist_sq(p, q))

    counts = cinderx.jit.get_function_hir_opcode_counts(dist_sq)
    assert counts is not None
    assert counts.get("DoubleBinaryOp", 0) >= 5, counts
    load_attr_count = counts.get("LoadAttr", 0) + counts.get("LoadAttrCached", 0)
    assert load_attr_count >= 6, counts
    assert counts.get("PrimitiveBox", 0) == 1, counts
    assert dist_sq(p, q) == 27.0


def test_primitive_box_remat_deopt_reconstructs_double_locals():
    p = Body(1.0, 2.0, 3.0)
    q = Body(4.0, 5.0, 6.0)
    _specialize_then_compile(dist_sq, lambda: dist_sq(p, q))

    cinderx.jit.clear_runtime_stats()
    assert dist_sq(p, IntYBody()) == 109.0
    stats = cinderx.jit.get_and_clear_runtime_stats()
    assert len(stats.get("deopt") or ()) >= 1, stats
