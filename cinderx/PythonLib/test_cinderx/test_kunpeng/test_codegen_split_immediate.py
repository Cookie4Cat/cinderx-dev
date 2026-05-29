import unittest
from typing import Callable

import cinderx.jit
from cinderx.jit import force_compile, force_uncompile, jit_suppress, jit_unsuppress


SPLIT_IMM = (2 << 12) + 1
MAX_SPLIT_IMM = (0xFFF << 12) + 0xFFF
UNSPLIT_IMM = 1 << 24


def add_split_immediate(value: int) -> int:
    return value + SPLIT_IMM


def sub_split_immediate(value: int) -> int:
    return value - SPLIT_IMM


def sub_max_split_immediate(value: int) -> int:
    return value - MAX_SPLIT_IMM


def eq_split_immediate(value: int) -> bool:
    return value == SPLIT_IMM


def eq_negative_split_immediate(value: int) -> bool:
    return value == -SPLIT_IMM


def ne_split_immediate(value: int) -> bool:
    return value != SPLIT_IMM


def lt_split_immediate(value: int) -> bool:
    return value < SPLIT_IMM


def add_unsplit_immediate(value: int) -> int:
    return value + UNSPLIT_IMM


class CodegenSplitImmediateTests(unittest.TestCase):
    def assert_jit_matches_interpreter(
        self,
        func: Callable[[int], object],
        values: list[int],
    ) -> None:
        if not cinderx.jit.is_enabled():
            return

        force_uncompile(func)
        jit_suppress(func)
        try:
            expected = [func(value) for value in values]
        finally:
            jit_unsuppress(func)

        self.assertTrue(force_compile(func))
        self.assertTrue(cinderx.jit.is_jit_compiled(func))
        self.assertEqual([func(value) for value in values], expected)

    def test_add_split_immediate_matches_interpreter(self) -> None:
        self.assert_jit_matches_interpreter(
            add_split_immediate,
            [-SPLIT_IMM, -1, 0, 1, SPLIT_IMM, SPLIT_IMM + 3],
        )

    def test_sub_split_immediate_matches_interpreter(self) -> None:
        self.assert_jit_matches_interpreter(
            sub_split_immediate,
            [-SPLIT_IMM, -1, 0, 1, SPLIT_IMM, SPLIT_IMM + 3],
        )
        self.assert_jit_matches_interpreter(
            sub_max_split_immediate,
            [-MAX_SPLIT_IMM, -1, 0, 1, MAX_SPLIT_IMM],
        )

    def test_equality_split_immediate_matches_interpreter(self) -> None:
        self.assert_jit_matches_interpreter(
            eq_split_immediate,
            [SPLIT_IMM - 1, SPLIT_IMM, SPLIT_IMM + 1],
        )
        self.assert_jit_matches_interpreter(
            ne_split_immediate,
            [SPLIT_IMM - 1, SPLIT_IMM, SPLIT_IMM + 1],
        )
        self.assert_jit_matches_interpreter(
            eq_negative_split_immediate,
            [-SPLIT_IMM - 1, -SPLIT_IMM, -SPLIT_IMM + 1],
        )

    def test_ordering_split_immediate_matches_interpreter(self) -> None:
        self.assert_jit_matches_interpreter(
            lt_split_immediate,
            [SPLIT_IMM - 1, SPLIT_IMM, SPLIT_IMM + 1],
        )

    def test_unsplit_immediate_fallback_matches_interpreter(self) -> None:
        self.assert_jit_matches_interpreter(
            add_unsplit_immediate,
            [-UNSPLIT_IMM, -1, 0, 1, UNSPLIT_IMM],
        )


if __name__ == "__main__":
    unittest.main()
