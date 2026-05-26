"""
Tests for array.array('d') BINARY_SUBSCR fast path specialization.

Verifies that the JIT compiles array double load operations correctly
and falls back to the slow path for mismatched types.
"""

import cinderx.jit
import pytest

from array import array


def _compile_func(func, runner):
    """Warm up and force-compile a function via the JIT."""
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(20):
            runner()
        cinderx.jit.jit_unsuppress(func)
        assert cinderx.jit.force_compile(func)
    finally:
        cinderx.jit.jit_unsuppress(func)


@pytest.fixture(autouse=True)
def _require_jit():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")


# ---------------------------------------------------------------------------
# Basic correctness
# ---------------------------------------------------------------------------


def test_array_double_load_correctness():
    """a[i] from array('d') returns the correct float after JIT compilation."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0))

    arr = array("d", [1.5, 2.5, 3.5])
    assert f(arr, 0) == 1.5
    assert f(arr, 1) == 2.5
    assert f(arr, 2) == 3.5


def test_array_double_load_negative_index():
    """Negative index loads from the correct position."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0))

    arr = array("d", [10.0, 20.0, 30.0])
    assert f(arr, -1) == 30.0
    assert f(arr, -2) == 20.0


def test_array_double_load_loop():
    """Loading array('d') elements in a loop returns correct values."""

    def f(a, n):
        total = 0.0
        for i in range(n):
            total += a[i]
        return total

    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 3))

    arr = array("d", [1.5, 2.5, 3.5])
    assert f(arr, 3) == 7.5


# ---------------------------------------------------------------------------
# Slow path fallback
# ---------------------------------------------------------------------------


def test_array_double_load_list_fallback():
    """Loading from a list falls back to slow path and works correctly."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0]), 0))

    lst = [10, 20, 30]
    assert f(lst, 1) == 20


def test_array_double_load_non_d_typecode_fallback():
    """Loading from array('f') falls back to slow path and works correctly."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0]), 0))

    arr_f = array("f", [1.0, 2.0])
    assert f(arr_f, 0) == 1.0


def test_array_double_load_tuple_fallback():
    """Loading from a tuple falls back to slow path and works correctly."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0]), 0))

    assert f((10, 20, 30), 2) == 30


# ---------------------------------------------------------------------------
# Bounds checking
# ---------------------------------------------------------------------------


def test_array_double_load_out_of_bounds():
    """Out-of-bounds load raises IndexError."""

    def f(a, i):
        return a[i]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0]), 0))

    arr = array("d", [1.0, 2.0])
    with pytest.raises(IndexError):
        f(arr, 10)
    with pytest.raises(IndexError):
        f(arr, -10)


# ---------------------------------------------------------------------------
# Load -> arithmetic chain (box-unbox elimination target)
# ---------------------------------------------------------------------------


def test_array_double_load_arithmetic_chain():
    """Load results can be used in arithmetic without correctness issues."""

    def f(a):
        return a[0] + a[1]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0])))

    arr = array("d", [1.5, 2.5])
    assert f(arr) == 4.0


def test_array_double_load_store_chain():
    """load -> compute -> store chain works end-to-end."""

    def f(dst, src, n):
        for i in range(n):
            dst[i] = src[i] * 2.0

    _compile_func(
        f, lambda: f(array("d", [0.0, 0.0]), array("d", [1.0, 2.0]), 2)
    )

    dst = array("d", [0.0, 0.0])
    src = array("d", [3.0, 5.0])
    f(dst, src, 2)
    assert list(dst) == [6.0, 10.0]


# ---------------------------------------------------------------------------
# Slice subscript fallback (crash regression test)
# ---------------------------------------------------------------------------


def test_array_double_load_slice_subscript():
    """Loading with a slice subscript does not crash (was SIGSEGV before fix).

    The JIT fast path uses CondBranchCheckType for the index guard, which
    routes non-int indices (slices) to the generic slow path rather than
    deopting with a corrupted interpreter stack.
    """

    def f(a):
        return a[:]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0])))

    arr = array("d", [10.0, 20.0, 30.0])
    result = f(arr)
    assert list(result) == [10.0, 20.0, 30.0]


def test_array_double_load_mixed_int_and_slice():
    """Integer and slice subscripts can be used interchangeably."""

    def f(a, use_slice):
        if use_slice:
            return a[:]
        return a[0]

    _compile_func(f, lambda: f(array("d", [1.0, 2.0]), False))

    arr = array("d", [10.0, 20.0])
    assert f(arr, False) == 10.0
    assert list(f(arr, True)) == [10.0, 20.0]
