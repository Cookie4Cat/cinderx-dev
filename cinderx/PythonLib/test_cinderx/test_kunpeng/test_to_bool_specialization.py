# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys
import unittest
from typing import Callable

import cinderx
import cinderx.jit
from cinderx.test_support import passIf
from test_cinderx.test_jit_specialization import opnames, specialize

HIRCounts = dict[str, int]


def warmup(call: Callable[[], object]) -> object:
    result = None
    for _ in range(20):
        result = call()
    return result


def compile_to_bool_with(value: object) -> Callable[[object], int]:
    def f(x: object) -> int:
        if x:
            return 1
        return 0

    specialize(f, lambda: warmup(lambda: f(value)))
    return f


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@passIf(sys.version_info < (3, 14), "Requires Python 3.14 TO_BOOL specialization")
class ToBoolSpecializationTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def assertHasSpecializedOpcode(
        self, func: Callable[[object], int], opcode: str
    ) -> None:
        actual_opnames = opnames(func)
        self.assertIn(opcode, actual_opnames, actual_opnames)

    def assertDoesNotHaveBuiltinToBoolOpcode(
        self, func: Callable[[object], int]
    ) -> None:
        actual_opnames = opnames(func)
        self.assertNotIn("TO_BOOL_BOOL", actual_opnames, actual_opnames)
        self.assertNotIn("TO_BOOL_INT", actual_opnames, actual_opnames)
        self.assertNotIn("TO_BOOL_LIST", actual_opnames, actual_opnames)
        self.assertNotIn("TO_BOOL_STR", actual_opnames, actual_opnames)

    def finalHirCounts(self, func: Callable[[object], int]) -> HIRCounts:
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        if counts is None:
            self.fail("function was not JIT compiled")
        return counts

    def assertSpecializedToBoolHir(
        self, func: Callable[[object], int]
    ) -> None:
        counts = self.finalHirCounts(func)
        self.assertIn("GuardType", counts, counts)
        self.assertEqual(counts.get("IsTruthy", 0), 0, counts)
        self.assertEqual(counts.get("PrimitiveBoxBool", 0), 0, counts)

    def test_001_to_bool_list_specialized_opcode_and_length_hir(self) -> None:
        f = compile_to_bool_with([1])

        self.assertHasSpecializedOpcode(f, "TO_BOOL_LIST")
        self.assertSpecializedToBoolHir(f)
        self.assertEqual(f([1]), 1)
        self.assertEqual(f([]), 0)

    def test_002_to_bool_str_specialized_opcode_and_length_hir(self) -> None:
        f = compile_to_bool_with("x")

        self.assertHasSpecializedOpcode(f, "TO_BOOL_STR")
        self.assertSpecializedToBoolHir(f)
        self.assertEqual(f("x"), 1)
        self.assertEqual(f(""), 0)

    def test_003_to_bool_bool_specialized_opcode_and_bool_hir(self) -> None:
        f = compile_to_bool_with(True)

        self.assertHasSpecializedOpcode(f, "TO_BOOL_BOOL")
        self.assertSpecializedToBoolHir(f)
        self.assertEqual(f(True), 1)
        self.assertEqual(f(False), 0)

    def test_004_to_bool_int_specialized_opcode_and_zero_hir(self) -> None:
        f = compile_to_bool_with(1)

        self.assertHasSpecializedOpcode(f, "TO_BOOL_INT")
        self.assertSpecializedToBoolHir(f)
        self.assertEqual(f(1), 1)
        self.assertEqual(f(0), 0)
        self.assertEqual(f(2**100), 1)
        self.assertEqual(f(-(2**100)), 1)

    def test_005_to_bool_exception_propagates(self) -> None:
        calls: list[str] = []

        class C:
            def __bool__(self) -> bool:
                calls.append("bool")
                raise RuntimeError("boom")

        f = compile_to_bool_with(True)

        with self.assertRaisesRegex(RuntimeError, "boom"):
            f(C())
        self.assertEqual(calls, ["bool"])

    def test_006_to_bool_int_warmup_type_change_keeps_semantics(self) -> None:
        calls: list[bool] = []

        class C:
            def __init__(self, value: bool) -> None:
                self.value = value

            def __bool__(self) -> bool:
                calls.append(self.value)
                return self.value

        f = compile_to_bool_with(1)

        self.assertHasSpecializedOpcode(f, "TO_BOOL_INT")
        self.assertEqual(f(C(True)), 1)
        self.assertEqual(f(C(False)), 0)
        self.assertEqual(f(""), 0)
        self.assertEqual(calls, [True, False])

    def test_007_custom_bool_object_stays_on_generic_to_bool_path(self) -> None:
        calls: list[str] = []

        class C:
            def __bool__(self) -> bool:
                calls.append("bool")
                return True

        obj = C()
        f = compile_to_bool_with(obj)

        self.assertDoesNotHaveBuiltinToBoolOpcode(f)

        calls.clear()
        self.assertEqual(f(obj), 1)
        self.assertEqual(calls, ["bool"])

    def test_to_bool_bool_miss_uses_python_bool_semantics(self) -> None:
        calls: list[str] = []

        class C:
            def __bool__(self) -> bool:
                calls.append("bool")
                return False

        f = compile_to_bool_with(True)

        self.assertIn("TO_BOOL_BOOL", opnames(f))
        self.assertEqual(f(C()), 0)
        self.assertEqual(calls, ["bool"])

    def test_to_bool_list_miss_uses_python_len_semantics(self) -> None:
        calls: list[str] = []

        class C:
            def __len__(self) -> int:
                calls.append("len")
                return 2

        f = compile_to_bool_with([1])

        self.assertIn("TO_BOOL_LIST", opnames(f))
        self.assertEqual(f(C()), 1)
        self.assertEqual(calls, ["len"])


if __name__ == "__main__":
    unittest.main()
