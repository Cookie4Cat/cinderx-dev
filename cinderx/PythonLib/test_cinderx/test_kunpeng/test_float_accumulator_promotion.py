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


def _make_data_accumulator():
    def func(data):
        s = 0
        for x in data:
            s += x
        return s

    return func


def _make_mixed_accumulator():
    def func(data, initial):
        s = initial
        for x in data:
            s += x
        return s

    return func


def _interpreted_result(factory, *args):
    func = factory()
    jit_suppress(func)
    try:
        return func(*args)
    finally:
        jit_unsuppress(func)


def _assert_mixed_accumulator_was_not_promoted(func):
    opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
    assert opcodes is not None
    # A generic specialized float add can still leave a DoubleBinaryOp here.
    # The promotion-specific fingerprint is the synthetic 0.0 LoadConst and
    # extra promoted Phi that remove the accumulator GuardType.
    assert opcodes.get("GuardType", 0) >= 2
    assert opcodes.get("Phi", 0) == 2
    assert opcodes.get("LoadConst", 0) == 1


def _specialize_then_compile(func, *warmup_args, warmup_calls=None):
    if not warmup_args:
        warmup_args = (10,)
    if warmup_calls is None:
        warmup_calls = [warmup_args] * 20

    # Run the function under the interpreter long enough for CPython's
    # adaptive specializer to rewrite ``BINARY_OP`` into the float fast
    # path before the JIT picks it up.
    force_uncompile(func)
    jit_suppress(func)
    try:
        for args in warmup_calls:
            func(*args)
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


def test_data_float_accumulator_matches_interpreter_without_repeated_deopt():
    func = _make_data_accumulator()
    data = [1.0] * 1000
    _specialize_then_compile(func, data)

    expected = _interpreted_result(_make_data_accumulator, data)
    cinderx.jit.get_and_clear_runtime_stats()
    for _ in range(3):
        assert func(data) == expected

    stats = cinderx.jit.get_and_clear_runtime_stats()
    assert stats["deopt"] == []

    opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
    assert opcodes is not None
    assert opcodes.get("DoubleBinaryOp", 0) >= 1


def test_data_float_accumulator_empty_data_returns_int_zero():
    func = _make_data_accumulator()
    _specialize_then_compile(func, [1.0] * 1000)

    expected = _interpreted_result(_make_data_accumulator, [])
    result = func([])
    assert result == expected
    assert type(result) is int

    opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
    assert opcodes is not None
    assert opcodes.get("DoubleBinaryOp", 0) >= 1


def test_mixed_accumulator_initial_argument_keeps_generic_path():
    func = _make_mixed_accumulator()
    warmup_data = [1.0]
    warmup_calls = [(warmup_data, 1), (warmup_data, 0.5)] * 10
    _specialize_then_compile(func, warmup_calls=warmup_calls)
    _assert_mixed_accumulator_was_not_promoted(func)

    for data, initial in (
        ([1.0] * 1000, 1),
        ([1.0] * 1000, 0.5),
        ([], 1),
        ([], 0.5),
    ):
        expected = _interpreted_result(_make_mixed_accumulator, data, initial)
        result = func(data, initial)
        assert result == expected
        assert type(result) is type(expected)
