import pytest

import cinderx.jit
from cinderx.jit import (
    force_compile,
    force_uncompile,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
)


@pytest.fixture(autouse=True)
def _require_jit_with_specialization():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")
    cinderx.jit.enable_specialized_opcodes()


def _make_accumulator():
    def func(n):
        s = 0
        for _ in range(n):
            s += 1.0
        return s

    return func


def _specialize_then_compile(func, warmup_arg=10):
    # Run the function under the interpreter long enough for CPython's
    # adaptive specializer to rewrite ``BINARY_OP`` into the float fast
    # path before the JIT picks it up.
    force_uncompile(func)
    jit_suppress(func)
    try:
        for _ in range(20):
            func(warmup_arg)
    finally:
        jit_unsuppress(func)
    assert force_compile(func)
    assert is_jit_compiled(func)


def test_float_accumulator_promotes_zero_entry_without_deopt():
    func = _make_accumulator()
    _specialize_then_compile(func)

    cinderx.jit.get_and_clear_runtime_stats()
    assert func(1000) == 1000.0

    stats = cinderx.jit.get_and_clear_runtime_stats()
    assert stats["deopt"] == []

    opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
    assert opcodes is not None
    assert opcodes.get("DoubleBinaryOp") == 1


def test_float_accumulator_empty_loop_still_returns_int_zero():
    func = _make_accumulator()
    _specialize_then_compile(func)

    result = func(0)
    assert result == 0
    assert type(result) is int


def test_float_accumulator_repeated_calls_remain_deopt_free():
    func = _make_accumulator()
    _specialize_then_compile(func)

    cinderx.jit.get_and_clear_runtime_stats()
    expected = [10.0, 100.0, 1000.0]
    observed = [func(10), func(100), func(1000)]
    assert observed == expected

    stats = cinderx.jit.get_and_clear_runtime_stats()
    assert stats["deopt"] == []
