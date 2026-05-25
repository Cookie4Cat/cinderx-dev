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
