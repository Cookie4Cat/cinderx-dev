import sys

import cinderx.jit
from cinderx.jit import (
    force_compile,
    force_uncompile,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
)


def make_accumulator():
    def func(n):
        s = 0
        for _ in range(n):
            s += 1.0
        return s

    return func


def make_data_accumulator():
    def func(data):
        s = 0
        for x in data:
            s += x
        return s

    return func


def make_mixed_accumulator():
    def func(data, initial):
        s = initial
        for x in data:
            s += x
        return s

    return func


def interpreted_result(factory, *args):
    func = factory()
    jit_suppress(func)
    try:
        return func(*args)
    finally:
        jit_unsuppress(func)


def specialize_then_compile(func, *warmup_args, warmup_calls=None):
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


def run_mixed_hir_case() -> None:
    cinderx.jit.enable_specialized_opcodes()
    func = make_mixed_accumulator()
    warmup_data = [1.0]
    warmup_calls = [(warmup_data, 1), (warmup_data, 0.5)] * 10
    specialize_then_compile(func, warmup_calls=warmup_calls)
    print("CASE_RESULT mixed_accumulator compiled")


def main() -> int:
    if sys.argv[1:] != ["mixed"]:
        raise SystemExit(f"usage: {sys.argv[0]} mixed")
    run_mixed_hir_case()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
