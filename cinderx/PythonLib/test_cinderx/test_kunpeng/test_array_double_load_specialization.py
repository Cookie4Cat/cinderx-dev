"""
Tests for array.array('d') BINARY_SUBSCR fast path specialization.

Verifies that the JIT compiles array double load operations correctly
and falls back to the slow path for mismatched types.
"""

import unittest
from array import array

import cinderx.jit


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class ArrayDoubleLoadSpecializationTests(unittest.TestCase):
    def setUp(self):
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def _compile_func(self, func, runner):
        """Warm up and force-compile a function via the JIT."""
        cinderx.jit.force_uncompile(func)
        cinderx.jit.jit_suppress(func)
        try:
            for _ in range(20):
                runner()
            cinderx.jit.jit_unsuppress(func)
            self.assertTrue(cinderx.jit.force_compile(func))
        finally:
            cinderx.jit.jit_unsuppress(func)

    # -----------------------------------------------------------------------
    # Basic correctness
    # -----------------------------------------------------------------------

    def test_array_double_load_correctness(self):
        """a[i] from array('d') returns the correct float after JIT compilation."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0))

        arr = array("d", [1.5, 2.5, 3.5])
        self.assertEqual(f(arr, 0), 1.5)
        self.assertEqual(f(arr, 1), 2.5)
        self.assertEqual(f(arr, 2), 3.5)

    def test_array_double_load_negative_index(self):
        """Negative index loads from the correct position."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0))

        arr = array("d", [10.0, 20.0, 30.0])
        self.assertEqual(f(arr, -1), 30.0)
        self.assertEqual(f(arr, -2), 20.0)

    def test_array_double_load_loop(self):
        """Loading array('d') elements in a loop returns correct values."""

        def f(a, n):
            total = 0.0
            for i in range(n):
                total += a[i]
            return total

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 3))

        arr = array("d", [1.5, 2.5, 3.5])
        self.assertEqual(f(arr, 3), 7.5)

    def test_array_double_load_fast_path_emitted_in_hir(self):
        """array('d') load compilation emits LoadArrayItem."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0))
        counts = cinderx.jit.get_function_hir_opcode_counts(f)
        self.assertGreater(counts.get("LoadArrayItem", 0), 0)

    # -----------------------------------------------------------------------
    # Slow path fallback
    # -----------------------------------------------------------------------

    def test_array_double_load_list_fallback(self):
        """Loading from a list falls back to slow path and works correctly."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0]), 0))

        lst = [10, 20, 30]
        self.assertEqual(f(lst, 1), 20)

    def test_array_double_load_non_d_typecode_fallback(self):
        """Loading from array('f') falls back to slow path and works correctly."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0]), 0))

        arr_f = array("f", [1.0, 2.0])
        self.assertEqual(f(arr_f, 0), 1.0)

    def test_array_double_load_tuple_fallback(self):
        """Loading from a tuple falls back to slow path and works correctly."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0]), 0))

        self.assertEqual(f((10, 20, 30), 2), 30)

    # -----------------------------------------------------------------------
    # Bounds checking
    # -----------------------------------------------------------------------

    def test_array_double_load_out_of_bounds(self):
        """Out-of-bounds load raises IndexError."""

        def f(a, i):
            return a[i]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0]), 0))

        arr = array("d", [1.0, 2.0])
        with self.assertRaises(IndexError):
            f(arr, 10)
        with self.assertRaises(IndexError):
            f(arr, -10)

    # -----------------------------------------------------------------------
    # Load -> arithmetic chain (box-unbox elimination target)
    # -----------------------------------------------------------------------

    def test_array_double_load_arithmetic_chain(self):
        """Load results can be used in arithmetic without correctness issues."""

        def f(a):
            return a[0] + a[1]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0])))

        arr = array("d", [1.5, 2.5])
        self.assertEqual(f(arr), 4.0)

    def test_array_double_load_store_chain(self):
        """load -> compute -> store chain works end-to-end."""

        def f(dst, src, n):
            for i in range(n):
                dst[i] = src[i] * 2.0

        self._compile_func(
            f,
            lambda: f(array("d", [0.0, 0.0]), array("d", [1.0, 2.0]), 2),
        )

        dst = array("d", [0.0, 0.0])
        src = array("d", [3.0, 5.0])
        f(dst, src, 2)
        self.assertEqual(list(dst), [6.0, 10.0])

    # -----------------------------------------------------------------------
    # Slice subscript fallback (crash regression test)
    # -----------------------------------------------------------------------

    def test_array_double_load_slice_subscript(self):
        """Loading with a slice subscript does not crash (was SIGSEGV before fix).

        The JIT fast path uses CondBranchCheckType for the index guard, which
        routes non-int indices (slices) to the generic slow path rather than
        deopting with a corrupted interpreter stack.
        """

        def f(a):
            return a[:]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0])))

        arr = array("d", [10.0, 20.0, 30.0])
        result = f(arr)
        self.assertEqual(list(result), [10.0, 20.0, 30.0])

    def test_array_double_load_mixed_int_and_slice(self):
        """Integer and slice subscripts can be used interchangeably."""

        def f(a, use_slice):
            if use_slice:
                return a[:]
            return a[0]

        self._compile_func(f, lambda: f(array("d", [1.0, 2.0]), False))

        arr = array("d", [10.0, 20.0])
        self.assertEqual(f(arr, False), 10.0)
        self.assertEqual(list(f(arr, True)), [10.0, 20.0])
