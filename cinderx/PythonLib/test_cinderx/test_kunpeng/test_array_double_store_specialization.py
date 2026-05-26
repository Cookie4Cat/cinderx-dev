"""
Tests for array.array('d') STORE_SUBSCR fast path specialization.

Verifies that the JIT compiles array double store operations correctly
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


def test_array_double_store_correctness():
    """array('d')[i] = float_val produces correct value after JIT compilation."""

    def f(a, i, v):
        a[i] = v

    arr = array("d", [1.0, 2.0, 3.0])
    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0, 4.0))

    f(arr, 1, 99.5)
    assert arr[1] == 99.5


def test_array_double_store_negative_index():
    """Negative index stores to the correct position."""

    def f(a, i, v):
        a[i] = v

    arr = array("d", [10.0, 20.0, 30.0])
    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 0, 4.0))

    f(arr, -1, 77.0)
    assert arr[2] == 77.0


def test_array_double_store_overwrite():
    """Overwriting an existing element works correctly."""

    def f(a, i, v):
        a[i] = v

    arr = array("d", [1.0, 2.0])
    _compile_func(f, lambda: f(array("d", [1.0, 2.0]), 0, 3.0))

    f(arr, 0, 10.0)
    assert arr[0] == 10.0
    f(arr, 1, 20.0)
    assert arr[1] == 20.0


# ---------------------------------------------------------------------------
# Slow path fallback
# ---------------------------------------------------------------------------


def test_array_double_store_list_fallback():
    """Storing to a list falls back to slow path and works correctly."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0]), 0, 2.0))

    lst = [1, 2, 3]
    f(lst, 1, 99)
    assert lst == [1, 99, 3]


def test_array_double_store_float_value():
    """Storing a float value to array('d') via fast path."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0]), 0, 2.0))

    arr = array("d", [1.0])
    f(arr, 0, float(42))
    assert arr[0] == 42.0


def test_array_double_store_tuple_fallback():
    """Storing to a tuple raises TypeError via slow path."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0]), 0, 2.0))

    with pytest.raises(TypeError):
        f((1, 2, 3), 0, 99)


def test_array_double_store_int_value_coercion():
    """Storing a bare int to array('d') falls back to slow path for coercion."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0]), 0, 2.0))

    arr = array("d", [0.0])
    f(arr, 0, 42)
    assert arr[0] == 42.0


def test_array_double_store_non_d_typecode_fallback():
    """Storing to array('i') falls back to slow path after 'd' warmup."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0]), 0, 2.0))

    arr_i = array("i", [1, 2, 3])
    f(arr_i, 1, 99)
    assert arr_i[1] == 99


# ---------------------------------------------------------------------------
# Bounds checking
# ---------------------------------------------------------------------------


def test_array_double_store_out_of_bounds():
    """Out-of-bounds store raises IndexError."""

    def f(a, i, v):
        a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0, 2.0]), 0, 3.0))

    arr = array("d", [1.0, 2.0])
    with pytest.raises(IndexError):
        f(arr, 10, 5.0)
    with pytest.raises(IndexError):
        f(arr, -10, 5.0)


def test_array_double_store_loop():
    """array(d)[i] = float_val works correctly in a loop after JIT compilation."""

    def f(a, n, v):
        for i in range(n):
            a[i] = v

    _compile_func(f, lambda: f(array("d", [1.0, 2.0, 3.0]), 3, 4.0))

    arr = array("d", [0.0, 0.0, 0.0])
    f(arr, 3, 7.5)
    assert list(arr) == [7.5, 7.5, 7.5]


# ---------------------------------------------------------------------------
# Slice subscript fallback (crash regression test)
# ---------------------------------------------------------------------------


def test_array_double_store_slice_subscript():
    """Storing with a slice subscript does not crash (was SIGSEGV before fix).

    The JIT fast path uses CondBranchCheckType for the index guard, which
    routes non-int indices (slices) to the generic slow path rather than
    deopting with a corrupted interpreter stack.
    """

    def f(a, src):
        a[:] = src

    _compile_func(f, lambda: f(array("d", [1.0, 2.0]), array("d", [3.0, 4.0])))

    arr = array("d", [0.0, 0.0])
    f(arr, array("d", [10.0, 20.0]))
    assert list(arr) == [10.0, 20.0]


def test_array_double_store_mixed_int_and_slice():
    """Integer and slice subscripts can be used interchangeably."""

    def f(a, use_slice, v):
        if use_slice:
            a[:] = v
        else:
            a[0] = v

    _compile_func(f, lambda: f(array("d", [1.0]), False, 2.0))

    arr = array("d", [0.0, 0.0])
    f(arr, False, 5.0)
    assert arr[0] == 5.0
    f(arr, True, array("d", [99.0, 88.0]))
    assert list(arr) == [99.0, 88.0]
