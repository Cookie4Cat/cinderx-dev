import unittest
from typing import Any, Callable, TypeVar

import cinderx.jit


TCallableRet = TypeVar("TCallableRet")


def _specialize(
    func: Callable[..., TCallableRet],
    callable: Callable[[], TCallableRet],
    warmup: int = 20,
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(warmup):
            callable()
    finally:
        cinderx.jit.jit_unsuppress(func)
    assert cinderx.jit.force_compile(func)
    assert cinderx.jit.is_jit_compiled(func)


def _hir_opcodes(func: Callable[..., Any]) -> dict[str, int]:
    opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
    assert opcodes is not None
    return opcodes


def _numeric_leaf(i: Any, j: Any) -> Any:
    return (i + j) * (i - j)


def _numeric_leaf_caller(pairs: list[tuple[Any, Any]]) -> list[Any]:
    results = []
    for i, j in pairs:
        results.append(_numeric_leaf(i, j))
    return results


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class NumericLeafIntGuardTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_simple_numeric_leaf_gets_int_specialized_hir(self) -> None:
        def eval_a(i: Any, j: Any) -> Any:
            return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

        _specialize(eval_a, lambda: eval_a(3, 4))

        opcodes = _hir_opcodes(eval_a)
        self.assertGreater(opcodes.get("LongBinaryOp", 0), 0)
        self.assertEqual(opcodes.get("BinaryOp", 0), 0)
        self.assertEqual(eval_a(5, 6), 1.0 / ((5 + 6) * (5 + 6 + 1) // 2 + 5 + 1))

    def test_unicode_ops_do_not_enable_int_compare_guards(self) -> None:
        def mixed(a: Any, b: Any, x: Any, y: Any) -> Any:
            value = a + b
            value = value + "x"
            return x < y

        _specialize(mixed, lambda: mixed("a", "b", 1, 2))

        opcodes = _hir_opcodes(mixed)
        self.assertEqual(opcodes.get("LongCompare", 0), 0)
        self.assertGreater(opcodes.get("Compare", 0), 0)
        self.assertTrue(mixed("left", "right", 3, 4))

    @unittest.skipUnless(
        cinderx.jit.is_hir_inliner_enabled(), "requires HIR inliner"
    )
    def test_numeric_leaf_inlined_into_loop_and_deopts_for_floats(self) -> None:
        int_pairs = [(3, 1), (8, 2), (11, 4)]
        expected_ints = [(i + j) * (i - j) for i, j in int_pairs]

        # test_jitlist.py runs earlier in the aggregate suite and leaves an
        # explicit JIT list active. Make the callee eligible for dependency
        # preloading so this test does not depend on process-wide test order.
        cinderx.jit.append_jit_list(
            f"{_numeric_leaf.__module__}:{_numeric_leaf.__qualname__}"
        )
        cinderx.jit.force_uncompile(_numeric_leaf_caller)
        cinderx.jit.force_uncompile(_numeric_leaf)
        cinderx.jit.jit_suppress(_numeric_leaf_caller)
        cinderx.jit.jit_suppress(_numeric_leaf)
        try:
            for _ in range(20):
                self.assertEqual(_numeric_leaf_caller(int_pairs), expected_ints)
        finally:
            cinderx.jit.jit_unsuppress(_numeric_leaf)
            cinderx.jit.jit_unsuppress(_numeric_leaf_caller)

        self.assertTrue(cinderx.jit.force_compile(_numeric_leaf_caller))
        inline_stats = cinderx.jit.get_inlined_functions_stats(
            _numeric_leaf_caller
        )
        self.assertGreaterEqual(
            cinderx.jit.get_num_inlined_functions(_numeric_leaf_caller),
            1,
            msg=(
                f"inliner_enabled={cinderx.jit.is_hir_inliner_enabled()}, "
                f"inline_stats={inline_stats}"
            ),
        )

        opcodes = _hir_opcodes(_numeric_leaf_caller)
        self.assertGreater(opcodes.get("LongBinaryOp", 0), 0)
        self.assertEqual(opcodes.get("BinaryOp", 0), 0)
        self.assertEqual(_numeric_leaf_caller(int_pairs), expected_ints)

        float_pairs = [(3.5, 1.25), (8.0, 2.5), (11.75, 4.25)]
        expected_floats = [(i + j) * (i - j) for i, j in float_pairs]
        self.assertEqual(_numeric_leaf_caller(float_pairs), expected_floats)

    def test_attribute_numeric_leaf_keeps_backedge_gate(self) -> None:
        class Vec:
            def __init__(self, x: int, y: int, z: int) -> None:
                self.x = x
                self.y = y
                self.z = z

            def mustBeVector(self) -> "Vec":
                return self

            def dot(self, other: "Vec") -> int:
                other.mustBeVector()
                return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)

        left = Vec(1, 2, 3)
        right = Vec(4, 5, 6)
        _specialize(Vec.dot, lambda: left.dot(right))

        opcodes = _hir_opcodes(Vec.dot)
        self.assertEqual(opcodes.get("LongBinaryOp", 0), 0)
        self.assertGreater(opcodes.get("BinaryOp", 0), 0)
        self.assertEqual(left.dot(right), 32)


if __name__ == "__main__":
    unittest.main()
