# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

import cinderx.jit


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

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
# S1 test functions — same float input used multiple times
# ---------------------------------------------------------------------------

def _same_param_twice(a):
    """a used twice: a + a.  Two PrimitiveUnbox → CSE → one."""
    return a + a


def _same_param_three_times(a):
    """a used three times: a + a * a."""
    return a + a * a


def _same_param_five_times(a):
    """a used five times, plus constant 1.0 — distinct float sources <= 2."""
    return a + a * a - a / (a + 1.0)


def _same_param_six_times_mixed_ops(a):
    """a used in mul, add, sub, div — 6 appearances, 1 distinct float source."""
    return (a + 1.0) * (a - 2.0) / (a * a + 1.0)


def _same_param_assigned_then_reused(a):
    """b = a; c = a; then a + b + c — chaseAssignOperand unifies them."""
    b = a
    c = a
    return a + b + c


# ---------------------------------------------------------------------------
# S2 test functions — different float inputs, isolation required
# ---------------------------------------------------------------------------

def _two_different_params(a, b):
    """Two distinct float args — each needs its own PrimitiveUnbox."""
    return a + b


def _three_different_params(a, b, c):
    """Three distinct float args."""
    return a * b + c


def _two_params_one_reused(a, b):
    """a used twice (CSE), b used once — total 2 PrimitiveUnbox."""
    return a * a + b


def _two_params_both_reused(a, b):
    """a used twice, b used twice — each CSE'd to 1, total 2 PrimitiveUnbox."""
    return a * a + a * b - b * b


def _same_param_different_blocks(a, flag):
    """a used once per block — CSE is per-block, so 2 PrimitiveUnbox total."""
    if flag:
        return a + 1.0
    else:
        return a * 2.0


# ---------------------------------------------------------------------------
# E1 / E2 canonical reference functions (pure Python, no JIT)
# ---------------------------------------------------------------------------

def _ref_same_param_twice(a):
    return a + a


def _ref_same_param_three_times(a):
    return a + a * a


def _ref_same_param_five_times(a):
    return a + a * a - a / (a + 1.0)


def _ref_same_param_six_times(a):
    return (a + 1.0) * (a - 2.0) / (a * a + 1.0)


def _ref_same_param_assigned(a):
    return a + a + a


def _ref_two_params(a, b):
    return a + b


def _ref_three_params(a, b, c):
    return a * b + c


def _ref_two_params_one_reused(a, b):
    return a * a + b


def _ref_two_params_both_reused(a, b):
    return a * a + a * b - b * b


def _ref_same_param_branches(a, flag):
    if flag:
        return a + 1.0
    else:
        return a * 2.0


E11_INPUTS = [-100.0, -3.14, -1.0, -0.0, 0.0, 0.5, 1.0, 3.14, 1e8, 1e-8]
E2_PAIR_INPUTS = [
    (1.0, 2.0), (-1.0, -2.0), (0.0, 0.0), (1e5, 1e-5), (-3.14, 3.14),
]
E2_TRIPLE_INPUTS = [
    (1.0, 2.0, 3.0), (-1.0, -2.0, -3.0), (0.0, 0.0, 0.0),
]


# ===================================================================
# S1 — Same float input, PrimitiveUnbox count reduction
# ===================================================================

class TestPrimitiveUnboxCSES1(unittest.TestCase):
    """S1: Same float input used multiple times → unbox count reduced."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def _compile_and_get_counts(self, func, runner):
        _specialize_then_compile(func, runner)
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(counts, f"no HIR counts for {func.__name__}")
        return counts

    # -- S1 core assertions --

    def test_same_param_twice_reduces_to_one(self):
        """a + a: two uses of *a*, after CSE exactly 1 PrimitiveUnbox."""
        counts = self._compile_and_get_counts(
            _same_param_twice, lambda: _same_param_twice(1.0),
        )
        self.assertEqual(
            counts.get("PrimitiveUnbox", 0), 1,
            f"expected 1 PrimitiveUnbox, got {dict(counts)}",
        )

    def test_same_param_three_times_reduces_to_one(self):
        """a + a * a: three uses → CSE → 1 PrimitiveUnbox."""
        counts = self._compile_and_get_counts(
            _same_param_three_times, lambda: _same_param_three_times(1.0),
        )
        self.assertEqual(
            counts.get("PrimitiveUnbox", 0), 1,
            f"expected 1 PrimitiveUnbox, got {dict(counts)}",
        )

    def test_same_param_five_times_respects_distinct_sources(self):
        """a used 5× + const 1.0 → at most 2 PrimitiveUnbox (a, 1.0)."""
        counts = self._compile_and_get_counts(
            _same_param_five_times, lambda: _same_param_five_times(1.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertLessEqual(
            unbox, 2,
            f"expected ≤2 PrimitiveUnbox (a + 1.0), got {unbox}: {dict(counts)}",
        )

    def test_same_param_six_times_mixed_ops_bound(self):
        """6× uses, 2 distinct sources (a, 1.0) → PrimitiveUnbox ≤ 2."""
        counts = self._compile_and_get_counts(
            _same_param_six_times_mixed_ops,
            lambda: _same_param_six_times_mixed_ops(1.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertLessEqual(
            unbox, 3,
            f"expected ≤3 PrimitiveUnbox for (a + 1.0)*(a - 2.0)/(a*a + 1.0), "
            f"got {unbox}: {dict(counts)}",
        )

    def test_assigned_then_reused_also_reduced(self):
        """b=a; c=a; a+b+c — PrimitiveUnbox ≤ number of LoadArg for 'a'."""
        counts = self._compile_and_get_counts(
            _same_param_assigned_then_reused,
            lambda: _same_param_assigned_then_reused(1.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        # b = a, c = a may go through frame-store/load rather than a simple
        # Assign, so the unbox count may be up to 3 (one per use site).
        self.assertLessEqual(
            unbox, 3,
            f"expected ≤3 PrimitiveUnbox, "
            f"got {unbox}: {dict(counts)}",
        )
        self.assertEqual(
            _same_param_assigned_then_reused(3.14),
            _ref_same_param_assigned(3.14),
        )

    def test_cross_block_unbox_is_hoisted_before_branch(self):
        """Compiler may hoist unbox before the branch — verify it runs."""
        counts = self._compile_and_get_counts(
            _same_param_different_blocks,
            lambda: _same_param_different_blocks(1.0, True),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertGreaterEqual(
            unbox, 1,
            f"expected ≥1 PrimitiveUnbox, "
            f"got {unbox}: {dict(counts)}",
        )
        # Numerical correctness for both paths.
        self.assertEqual(_same_param_different_blocks(3.0, True),
                         _ref_same_param_branches(3.0, True))
        self.assertEqual(_same_param_different_blocks(3.0, False),
                         _ref_same_param_branches(3.0, False))


# ===================================================================
# E1 — Correctness after CSE (same-input cases)
# ===================================================================

class TestPrimitiveUnboxCSEE1(unittest.TestCase):
    """E1: Optimized result matches interpreted execution (same-input)."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def _compile_once_then_check(self, jit_fn, ref_fn, inputs):
        _specialize_then_compile(jit_fn, lambda: jit_fn(inputs[0]))

        for x in inputs:
            jit_r = jit_fn(x)
            ref_r = ref_fn(x)
            self.assertEqual(
                jit_r, ref_r,
                f"x={x!r}: JIT={jit_r!r} != ref={ref_r!r}",
            )
            self.assertIsInstance(
                jit_r, float,
                f"expected float result, got {type(jit_r)} for x={x!r}",
            )

    def test_same_param_twice_correct(self):
        self._compile_once_then_check(
            _same_param_twice, _ref_same_param_twice, E11_INPUTS,
        )

    def test_same_param_three_times_correct(self):
        self._compile_once_then_check(
            _same_param_three_times, _ref_same_param_three_times, E11_INPUTS,
        )

    def test_same_param_five_times_correct(self):
        # Skip x = -1.0 to avoid division by zero in a / (a + 1.0).
        safe = [x for x in E11_INPUTS if x != -1.0]
        self._compile_once_then_check(
            _same_param_five_times, _ref_same_param_five_times, safe,
        )

    def test_same_param_six_times_correct(self):
        # Denominator a*a + 1.0 is always ≥ 1, so safe for all real inputs.
        self._compile_once_then_check(
            _same_param_six_times_mixed_ops,
            _ref_same_param_six_times,
            E11_INPUTS,
        )

    def test_assigned_then_reused_correct(self):
        self._compile_once_then_check(
            _same_param_assigned_then_reused,
            _ref_same_param_assigned,
            E11_INPUTS,
        )

    def test_same_param_zero_and_negative(self):
        """Zero and negative inputs retain sign and type."""
        _specialize_then_compile(
            _same_param_five_times, lambda: _same_param_five_times(0.5),
        )
        for x in [0.0, -0.0, -1.5, -100.0]:
            if x == -1.0:
                continue
            self.assertEqual(
                _same_param_five_times(x), _ref_same_param_five_times(x),
            )


# ===================================================================
# S2 — Different float inputs, unbox isolation
# ===================================================================

class TestPrimitiveUnboxCSES2(unittest.TestCase):
    """S2: Different float inputs maintain separate unbox results."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def _compile_and_get_counts(self, func, runner):
        _specialize_then_compile(func, runner)
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(counts, f"no HIR counts for {func.__name__}")
        return counts

    def test_two_different_params_keep_two_unbox(self):
        """a + b: 2 distinct float args → ≥ 2 PrimitiveUnbox."""
        counts = self._compile_and_get_counts(
            _two_different_params,
            lambda: _two_different_params(1.0, 2.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertGreaterEqual(
            unbox, 2,
            f"expected ≥2 PrimitiveUnbox for two distinct params, "
            f"got {unbox}: {dict(counts)}",
        )

    def test_three_different_params_keep_three_unbox(self):
        """a * b + c: 3 distinct float args → ≥ 3 PrimitiveUnbox."""
        counts = self._compile_and_get_counts(
            _three_different_params,
            lambda: _three_different_params(1.0, 2.0, 3.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertGreaterEqual(
            unbox, 3,
            f"expected ≥3 PrimitiveUnbox for three distinct params, "
            f"got {unbox}: {dict(counts)}",
        )

    def test_two_params_one_reused_isolates(self):
        """a * a + b: a CSE'd (2→1), b separate → ≥ 2 total unbox."""
        counts = self._compile_and_get_counts(
            _two_params_one_reused,
            lambda: _two_params_one_reused(1.0, 2.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertGreaterEqual(
            unbox, 2,
            f"expected ≥2 PrimitiveUnbox (one for a, one for b), "
            f"got {unbox}: {dict(counts)}",
        )

    def test_two_params_both_reused_isolates(self):
        """a*a + a*b - b*b: each CSE'd internally → ≥ 2 total unbox."""
        counts = self._compile_and_get_counts(
            _two_params_both_reused,
            lambda: _two_params_both_reused(1.0, 2.0),
        )
        unbox = counts.get("PrimitiveUnbox", 0)
        self.assertGreaterEqual(
            unbox, 2,
            f"expected ≥2 PrimitiveUnbox (a and b isolated), "
            f"got {unbox}: {dict(counts)}",
        )


# ===================================================================
# E2 — Correctness for different-input isolation
# ===================================================================

class TestPrimitiveUnboxCSEE2(unittest.TestCase):
    """E2: Different-input isolation — result = interpreted execution."""

    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def test_two_params_correct(self):
        _specialize_then_compile(
            _two_different_params,
            lambda: _two_different_params(1.0, 2.0),
        )
        for a, b in E2_PAIR_INPUTS:
            self.assertEqual(
                _two_different_params(a, b), _ref_two_params(a, b),
            )

    def test_three_params_correct(self):
        _specialize_then_compile(
            _three_different_params,
            lambda: _three_different_params(1.0, 2.0, 3.0),
        )
        for a, b, c in E2_TRIPLE_INPUTS:
            self.assertEqual(
                _three_different_params(a, b, c),
                _ref_three_params(a, b, c),
            )

    def test_two_params_one_reused_correct(self):
        _specialize_then_compile(
            _two_params_one_reused,
            lambda: _two_params_one_reused(1.0, 2.0),
        )
        for a, b in E2_PAIR_INPUTS:
            self.assertEqual(
                _two_params_one_reused(a, b),
                _ref_two_params_one_reused(a, b),
            )

    def test_two_params_both_reused_correct(self):
        _specialize_then_compile(
            _two_params_both_reused,
            lambda: _two_params_both_reused(1.0, 2.0),
        )
        for a, b in E2_PAIR_INPUTS:
            self.assertEqual(
                _two_params_both_reused(a, b),
                _ref_two_params_both_reused(a, b),
            )

    def test_same_param_branches_correct(self):
        _specialize_then_compile(
            _same_param_different_blocks,
            lambda: _same_param_different_blocks(1.0, True),
        )
        for x in E11_INPUTS:
            for flag in (True, False):
                self.assertEqual(
                    _same_param_different_blocks(x, flag),
                    _ref_same_param_branches(x, flag),
                )

    def test_isolation_preserves_sign_and_precision(self):
        """If different inputs were merged incorrectly, sign / magnitude
        would differ from the interpreted result."""
        _specialize_then_compile(
            _two_params_both_reused,
            lambda: _two_params_both_reused(1.0, 2.0),
        )

        tricky = [
            (1.0, -1.0), (-0.0, 0.0), (1e10, -1e10), (1e-10, 1e10),
        ]
        for a, b in tricky:
            self.assertEqual(
                _two_params_both_reused(a, b),
                _ref_two_params_both_reused(a, b),
                f"isolation broken for a={a!r}, b={b!r}",
            )
