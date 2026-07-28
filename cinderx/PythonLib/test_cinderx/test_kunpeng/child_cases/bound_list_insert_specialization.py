import argparse

import cinderx.jit


def negative_index_parallel_precompile() -> None:
    cinderx.jit.enable_specialized_opcodes()

    def insert_negative(item):
        values = [1, 2, 3]
        insert = values.insert
        insert(-1, item)
        return values

    cinderx.jit.jit_suppress(insert_negative)
    try:
        for _ in range(20):
            insert_negative(0)
    finally:
        cinderx.jit.jit_unsuppress(insert_negative)

    cinderx.jit.lazy_compile(insert_negative)
    assert cinderx.jit.precompile_all(workers=2)
    assert cinderx.jit.is_jit_compiled(insert_negative)
    counts = cinderx.jit.get_function_hir_opcode_counts(insert_negative)
    assert counts.get("CallStatic", 0) == 2, counts
    assert counts.get("VectorCall", 0) == 0, counts
    assert insert_negative("x") == [1, 2, "x", 3]


CASES = {
    "negative-index-parallel-precompile": negative_index_parallel_precompile,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
