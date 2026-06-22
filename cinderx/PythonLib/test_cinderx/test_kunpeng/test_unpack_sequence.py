# Copyright (c) Meta Platforms, Inc. and affiliates.

import dis
import unittest
from typing import Callable, TypeVar

import cinderx
import cinderx.jit
from cinderx.test_support import (
    FREE_THREADING_BUILD,
    passIf,
    passUnless,
    skip_if_ft,
)


TCallableRet = TypeVar("TCallableRet")


_all_opnames: list[str] = dis.opname
if hasattr(dis, "_specialized_instructions"):
    _specialized_indices: list[int] = [
        index for index, name in enumerate(_all_opnames) if name.startswith("<")
    ]

    for index, name in zip(_specialized_indices, dis._specialized_instructions):
        _all_opnames[index] = name


def opnames(func: Callable[..., TCallableRet]) -> list[str]:
    bytecode = dis.Bytecode(func, adaptive=True)
    return [_all_opnames[insn.opcode] for insn in bytecode]


def specialize(
    func: Callable[..., TCallableRet], callable: Callable[[], TCallableRet]
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)

    for _ in range(5):
        callable()

    cinderx.jit.jit_unsuppress(func)
    cinderx.jit.force_compile(func)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class KunpengUnpackSequenceTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def assert_deopt_count(self, expected: int) -> None:
        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(len(stats.get("deopt") or ()), expected)

    def assert_jit_and_no_jit_equal(
        self,
        compiled: Callable[[], TCallableRet],
        interpreted: Callable[[], TCallableRet],
        expected_deopt_count: int = 1,
    ) -> None:
        cinderx.jit.clear_runtime_stats()
        compiled_result = compiled()
        stats = cinderx.jit.get_and_clear_runtime_stats()

        cinderx.jit.disable()
        try:
            interpreted_result = interpreted()
        finally:
            cinderx.jit.enable()
            cinderx.jit.enable_specialized_opcodes()

        self.assertEqual(compiled_result, interpreted_result)
        self.assertEqual(len(stats.get("deopt") or ()), expected_deopt_count)

    def test_unpack_sequence_works_without_specialized_opcodes(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

        def f(seq):
            a, b, c = seq
            return a + b + c

        cinderx.jit.force_compile(f)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f([1, 2, 3]), 6)
        self.assert_deopt_count(0)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f((1, 2, 3)), 6)
        self.assert_deopt_count(0)

    @passUnless(FREE_THREADING_BUILD, "Only tests free-threading behavior")
    def test_list_specialized_unpack_uses_slow_path_in_free_threading(self) -> None:
        def f(seq):
            a, b, c = seq
            return a + b + c

        class CustomIterable:
            def __iter__(self):
                yield 1
                yield 2
                yield 3

        specialize(f, lambda: f([1, 2, 3]))

        self.assertIn("UNPACK_SEQUENCE_LIST", opnames(f))
        ops = cinderx.jit.get_function_hir_opcode_counts(f)
        self.assertGreaterEqual(ops.get("UnpackSequence", 0), 1)
        self.assertGreaterEqual(ops.get("ReserveStack", 0), 1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(CustomIterable()), 6)
        self.assert_deopt_count(0)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_list_specialized_unpack_keeps_tuple_fast_path(self) -> None:
        def f(seq):
            a, b, c = seq
            return a + b + c

        specialize(f, lambda: f([1, 2, 3]))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_LIST", opnames(f))
        ops = cinderx.jit.get_function_hir_opcode_counts(f)
        self.assertEqual(ops.get("UnpackSequence", 0), 0)
        self.assertEqual(ops.get("ReserveStack", 0), 0)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertGreaterEqual(ops.get("LoadFieldAddress", 0), 1)
        self.assertEqual(f([1, 2, 3]), 6)
        self.assertEqual(f((1, 2, 3)), 6)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_list_specialized_unpack_deopts_for_negative_cases(self) -> None:
        def f(seq):
            a, b, c = seq
            return a + b + c

        class ListSubclass(list):
            pass

        class TupleSubclass(tuple):
            pass

        class CustomIterable:
            def __iter__(self):
                yield 1
                yield 2
                yield 3

        specialize(f, lambda: f([1, 2, 3]))

        self.assertIn("UNPACK_SEQUENCE_LIST", opnames(f))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f((1, 2, 3)), 6)
        self.assert_deopt_count(0)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(ListSubclass([1, 2, 3])), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(TupleSubclass((1, 2, 3))), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(CustomIterable()), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            f([1, 2])
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "too many values to unpack"):
            f([1, 2, 3, 4])
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(TypeError, "cannot unpack non-iterable int"):
            f(1)
        self.assert_deopt_count(1)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_tuple_specialized_unpack_keeps_list_fast_path(self) -> None:
        def f(seq):
            a, b, c = seq
            return a + b + c

        specialize(f, lambda: f((1, 2, 3)))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_TUPLE", opnames(f))
        ops = cinderx.jit.get_function_hir_opcode_counts(f)
        self.assertEqual(ops.get("UnpackSequence", 0), 0)
        self.assertEqual(ops.get("ReserveStack", 0), 0)
        self.assertGreaterEqual(ops.get("LoadFieldAddress", 0), 1)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertEqual(f((1, 2, 3)), 6)
        self.assertEqual(f([1, 2, 3]), 6)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_tuple_specialized_unpack_deopts_for_negative_cases(self) -> None:
        def f(seq):
            a, b, c = seq
            return a + b + c

        class ListSubclass(list):
            pass

        class TupleSubclass(tuple):
            pass

        class CustomIterable:
            def __iter__(self):
                yield 1
                yield 2
                yield 3

        specialize(f, lambda: f((1, 2, 3)))

        self.assertIn("UNPACK_SEQUENCE_TUPLE", opnames(f))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f([1, 2, 3]), 6)
        self.assert_deopt_count(0)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(ListSubclass([1, 2, 3])), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(TupleSubclass((1, 2, 3))), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(CustomIterable()), 6)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            f((1, 2))
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "too many values to unpack"):
            f((1, 2, 3, 4))
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(TypeError, "cannot unpack non-iterable int"):
            f(1)
        self.assert_deopt_count(1)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_two_tuple_specialized_unpack_keeps_list_fast_path(self) -> None:
        def f(seq):
            a, b = seq
            return a + b

        specialize(f, lambda: f((1, 2)))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_TWO_TUPLE", opnames(f))
        ops = cinderx.jit.get_function_hir_opcode_counts(f)
        self.assertEqual(ops.get("UnpackSequence", 0), 0)
        self.assertEqual(ops.get("ReserveStack", 0), 0)
        self.assertGreaterEqual(ops.get("LoadFieldAddress", 0), 1)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertEqual(f((1, 2)), 3)
        self.assertEqual(f([1, 2]), 3)

    @skip_if_ft("List unpack fast path is disabled in free-threading builds")
    def test_two_tuple_specialized_unpack_deopts_for_negative_cases(self) -> None:
        def f(seq):
            a, b = seq
            return a + b

        class ListSubclass(list):
            pass

        class TupleSubclass(tuple):
            pass

        class CustomIterable:
            def __iter__(self):
                yield 1
                yield 2

        specialize(f, lambda: f((1, 2)))

        self.assertIn("UNPACK_SEQUENCE_TWO_TUPLE", opnames(f))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f([1, 2]), 3)
        self.assert_deopt_count(0)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(ListSubclass([1, 2])), 3)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(TupleSubclass((1, 2))), 3)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(f(CustomIterable()), 3)
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            f((1,))
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(ValueError, "too many values to unpack"):
            f((1, 2, 3))
        self.assert_deopt_count(1)

        cinderx.jit.clear_runtime_stats()
        with self.assertRaisesRegex(TypeError, "cannot unpack non-iterable int"):
            f(1)
        self.assert_deopt_count(1)

    def test_tuple_specialized_unpack_deopt_preserves_side_effect_order(self) -> None:
        def f(seq, events):
            events.append("before")
            a, b, c = seq
            events.append(f"after:{a},{b},{c}")
            return len(events)

        class SideEffectIterable:
            def __init__(self, events):
                self.events = events

            def __iter__(self):
                self.events.append("iter")
                yield 1
                self.events.append("yielded:1")
                yield 2
                self.events.append("yielded:2")
                yield 3
                self.events.append("yielded:3")

        specialize(f, lambda: f((1, 2, 3), []))

        self.assertIn("UNPACK_SEQUENCE_TUPLE", opnames(f))

        def compiled():
            events = []
            result = f(SideEffectIterable(events), events)
            return result, events

        def interpreted():
            events = []
            result = f(SideEffectIterable(events), events)
            return result, events

        self.assert_jit_and_no_jit_equal(compiled, interpreted)
