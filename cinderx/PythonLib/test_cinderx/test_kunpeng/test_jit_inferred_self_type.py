# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import dis
import opcode
import sys
import unittest
from typing import Any, Callable

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf


class RaytraceVector:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)


class RaytraceChainVector:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)

    def scale(self, factor: float) -> "RaytraceChainVector":
        return RaytraceChainVector(
            factor * self.x, factor * self.y, factor * self.z
        )

    def magnitude(self) -> float:
        return self.dot(self) ** 0.5

    def normalized(self) -> "RaytraceChainVector":
        return self.scale(1.0 / self.magnitude())


class VectorLike:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z


class WrongReceiverVector:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)


class LateSubclassVector:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)


class ExistingSubclassVector:
    x: float
    y: float
    z: float

    def __init__(self, x: float, y: float, z: float) -> None:
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)


class ExistingSubVector(ExistingSubclassVector):
    pass


class CustomGetattributeVector:
    x: int
    accesses: int

    def __init__(self, x: int) -> None:
        self.x = x
        self.accesses = 0

    def __getattribute__(self, name: str) -> Any:
        if name == "x":
            accesses = object.__getattribute__(self, "accesses")
            assert isinstance(accesses, int)
            object.__setattr__(self, "accesses", accesses + 1)
        return object.__getattribute__(self, name)

    def read_x(self) -> int:
        return self.x


class StaticMethodVector:
    @staticmethod
    def dot(self: Any, other: Any) -> float:
        return (self.x * other.x) + (self.y * other.y) + (self.z * other.z)


def replace_bytecode(
    func: Callable[..., Any],
    ops: list[tuple[str, int]],
    names: tuple[str, ...],
) -> None:
    code = bytearray()
    for opname, arg in ops:
        op = opcode.opmap[opname]
        code.append(op)
        code.append(arg)
        inline_cache_entries = dis._inline_cache_entries
        cache_count = (
            inline_cache_entries.get(opname, 0)
            if isinstance(inline_cache_entries, dict)
            else inline_cache_entries[op]
        )
        for _ in range(cache_count):
            code.append(opcode.opmap["CACHE"])
            code.append(0)

    func.__code__ = func.__code__.replace(
        co_code=bytes(code),
        co_names=names,
        co_stacksize=2,
    )


class FusedSecondSelfVector:
    x: float

    def __init__(self, x: float) -> None:
        self.x = x

    def tuple_with_other(self, other: Any) -> tuple[Any, float]:
        return (other, self.x)


if sys.version_info >= (3, 14):
    replace_bytecode(
        FusedSecondSelfVector.tuple_with_other,
        [
            ("RESUME", 0),
            ("LOAD_FAST_LOAD_FAST", 0x10),
            ("LOAD_ATTR", 0),
            ("BUILD_TUPLE", 2),
            ("RETURN_VALUE", 0),
        ],
        ("x",),
    )


class CountingXDescriptor:
    def __get__(self, obj: Any, owner: type[Any]) -> float:
        obj.accesses += 1
        return 10.0

    def __set__(self, obj: Any, value: Any) -> None:
        obj.__dict__["x"] = value


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class InferredSelfTypeTests(unittest.TestCase):
    def compile(self, func: Callable[..., Any]) -> None:
        cinderx.jit.force_uncompile(func)
        self.assertTrue(cinderx.jit.force_compile(func))

    def test_raytrace_vector_dot_exact_self_does_not_deopt(self) -> None:
        self.compile(RaytraceVector.dot)
        ops = cinderx.jit.get_function_hir_opcode_counts(RaytraceVector.dot)
        self.assertGreaterEqual(ops.get("GuardType", 0), 1)

        checked = fail_if_deopt(RaytraceVector.dot)
        self.assertEqual(
            checked(RaytraceVector(1.0, 2.0, 3.0), RaytraceVector(4.0, 5.0, 6.0)),
            32.0,
        )

    def test_raytrace_method_chain_exact_self_does_not_deopt(self) -> None:
        self.compile(RaytraceChainVector.normalized)

        checked = fail_if_deopt(RaytraceChainVector.normalized)
        result = checked(RaytraceChainVector(3.0, 4.0, 0.0))
        self.assertAlmostEqual(result.x, 0.6)
        self.assertAlmostEqual(result.y, 0.8)
        self.assertAlmostEqual(result.z, 0.0)

    def test_wrong_receiver_deopts_but_preserves_python_semantics(self) -> None:
        self.compile(WrongReceiverVector.dot)
        checked = fail_if_deopt(WrongReceiverVector.dot)

        receiver = VectorLike(1.0, 2.0, 3.0)
        other = VectorLike(4.0, 5.0, 6.0)
        with self.assertRaisesRegex(RuntimeError, "Deopt occurred"):
            checked(receiver, other)
        self.assertEqual(WrongReceiverVector.dot(receiver, other), 32.0)

    def test_late_subclass_receiver_deopts_not_wrong_result(self) -> None:
        self.compile(LateSubclassVector.dot)

        class SubLateSubclassVector(LateSubclassVector):
            pass

        receiver = SubLateSubclassVector(1.0, 2.0, 3.0)
        other = VectorLike(4.0, 5.0, 6.0)
        checked = fail_if_deopt(LateSubclassVector.dot)
        with self.assertRaisesRegex(RuntimeError, "Deopt occurred"):
            checked(receiver, other)
        self.assertEqual(LateSubclassVector.dot(receiver, other), 32.0)

    def test_existing_subclass_receiver_does_not_get_exact_self_assumption(
        self,
    ) -> None:
        self.compile(ExistingSubclassVector.dot)
        ops = cinderx.jit.get_function_hir_opcode_counts(ExistingSubclassVector.dot)
        self.assertEqual(ops.get("GuardType", 0), 0)

        checked = fail_if_deopt(ExistingSubclassVector.dot)
        self.assertEqual(
            checked(ExistingSubVector(1.0, 2.0, 3.0), VectorLike(4.0, 5.0, 6.0)),
            32.0,
        )

    def test_custom_getattribute_side_effect_preserved(self) -> None:
        self.compile(CustomGetattributeVector.read_x)
        ops = cinderx.jit.get_function_hir_opcode_counts(
            CustomGetattributeVector.read_x
        )
        self.assertEqual(ops.get("GuardType", 0), 0)

        receiver = CustomGetattributeVector(42)
        checked = fail_if_deopt(CustomGetattributeVector.read_x)
        self.assertEqual(checked(receiver), 42)
        self.assertEqual(receiver.accesses, 1)

    def test_staticmethod_descriptor_does_not_get_exact_self_assumption(
        self,
    ) -> None:
        self.compile(StaticMethodVector.dot)
        ops = cinderx.jit.get_function_hir_opcode_counts(StaticMethodVector.dot)
        self.assertEqual(ops.get("GuardType", 0), 0)

        checked = fail_if_deopt(StaticMethodVector.dot)
        self.assertEqual(
            checked(VectorLike(1.0, 2.0, 3.0), VectorLike(4.0, 5.0, 6.0)),
            32.0,
        )

    def test_fused_load_second_self_gets_exact_self_assumption(self) -> None:
        self.compile(FusedSecondSelfVector.tuple_with_other)
        ops = cinderx.jit.get_function_hir_opcode_counts(
            FusedSecondSelfVector.tuple_with_other
        )
        self.assertGreaterEqual(ops.get("GuardType", 0), 1)

        checked = fail_if_deopt(FusedSecondSelfVector.tuple_with_other)
        self.assertEqual(
            checked(FusedSecondSelfVector(3.5), "other"),
            ("other", 3.5),
        )

    def test_exact_self_missing_instance_attr_preserves_attribute_error(
        self,
    ) -> None:
        self.compile(RaytraceVector.dot)
        ops = cinderx.jit.get_function_hir_opcode_counts(RaytraceVector.dot)
        self.assertGreaterEqual(ops.get("GuardType", 0), 1)

        receiver = RaytraceVector(1.0, 2.0, 3.0)
        other = RaytraceVector(4.0, 5.0, 6.0)
        del receiver.x
        with self.assertRaises(AttributeError):
            RaytraceVector.dot(receiver, other)

    def test_exact_self_descriptor_replacement_preserves_lookup(self) -> None:
        self.compile(RaytraceVector.dot)
        ops = cinderx.jit.get_function_hir_opcode_counts(RaytraceVector.dot)
        self.assertGreaterEqual(ops.get("GuardType", 0), 1)

        receiver = RaytraceVector(1.0, 2.0, 3.0)
        other = RaytraceVector(4.0, 5.0, 6.0)
        receiver.accesses = 0
        other.accesses = 0
        try:
            RaytraceVector.x = CountingXDescriptor()
            self.assertEqual(RaytraceVector.dot(receiver, other), 128.0)
            self.assertEqual(receiver.accesses, 1)
            self.assertEqual(other.accesses, 1)
        finally:
            del RaytraceVector.x


if __name__ == "__main__":
    unittest.main()
