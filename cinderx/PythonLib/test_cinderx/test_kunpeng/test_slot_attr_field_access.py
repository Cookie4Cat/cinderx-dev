# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf, skip_if_ft
from test_cinderx.test_jit_specialization import opnames, specialize


class MemberDescriptorCounter:
    __slots__ = ("value",)


_member_descriptor_obj = MemberDescriptorCounter()
_member_descriptor_obj.value = 0


def store_member_descriptor_value(v: object) -> object:
    _member_descriptor_obj.value = v
    return _member_descriptor_obj.value


def store_readonly_member_descriptor_value(v: object) -> None:
    store_member_descriptor_value.__globals__ = v


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@skip_if_ft("slot attr field lowering is disabled with free-threading")
class SlotAttrFieldAccessTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def assert_deopt_count(self, expected: int) -> None:
        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(len(stats.get("deopt") or ()), expected)

    def assert_single_deopt(
        self, reason: str, description: str, lineno: int
    ) -> None:
        stats = cinderx.jit.get_and_clear_runtime_stats()
        deopts = stats.get("deopt") or ()
        self.assertEqual(len(deopts), 1)
        self.assertEqual(deopts[0]["normal"]["reason"], reason)
        self.assertEqual(deopts[0]["normal"]["description"], description)
        self.assertEqual(deopts[0]["int"]["lineno"], lineno)
        for key in (
            "bc_offset",
            "deopt_idx",
            "nonce",
            "opcode",
            "specialized_opcode",
        ):
            self.assertIsInstance(deopts[0]["int"][key], int)

    def test_member_descriptor_store_simplifies_to_store_field(self) -> None:
        cinderx.jit.force_uncompile(store_member_descriptor_value)
        _member_descriptor_obj.value = 0

        self.assertTrue(cinderx.jit.force_compile(store_member_descriptor_value))
        self.assertEqual(store_member_descriptor_value(7), 7)
        self.assertIsNone(store_member_descriptor_value(None))

        ops = cinderx.jit.get_function_hir_opcode_counts(store_member_descriptor_value)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertGreaterEqual(ops.get("StoreField", 0), 1)
        self.assertEqual(ops.get("StoreAttrCached", 0), 0)

    def test_readonly_member_descriptor_store_stays_generic(self) -> None:
        cinderx.jit.force_uncompile(store_readonly_member_descriptor_value)

        self.assertTrue(
            cinderx.jit.force_compile(store_readonly_member_descriptor_value)
        )
        ops = cinderx.jit.get_function_hir_opcode_counts(
            store_readonly_member_descriptor_value
        )
        self.assertEqual(ops.get("StoreField", 0), 0)
        with self.assertRaises(AttributeError):
            store_readonly_member_descriptor_value({})

    def test_slot_specialized_opcodes_lower_to_field_ops(self) -> None:
        class Counter:
            __slots__ = ("value",)

            def increment(self) -> None:
                self.value = self.value + 1

        counter = Counter()
        counter.value = 0

        specialize(Counter.increment, counter.increment)

        names = opnames(Counter.increment)
        self.assertIn("LOAD_ATTR_SLOT", names)
        self.assertIn("STORE_ATTR_SLOT", names)
        self.assertTrue(cinderx.jit.is_jit_compiled(Counter.increment))

        ops = cinderx.jit.get_function_hir_opcode_counts(Counter.increment)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertGreaterEqual(ops.get("StoreField", 0), 1)
        self.assertGreaterEqual(ops.get("LoadAttr", 0), 1)
        self.assertGreaterEqual(ops.get("PrimitiveCompare", 0), 1)
        self.assertGreaterEqual(ops.get("CondBranch", 0), 1)
        self.assertGreaterEqual(ops.get("Guard", 0), 1)
        self.assertGreaterEqual(ops.get("CheckField", 0), 1)
        self.assertEqual(ops.get("LoadAttrCached", 0), 0)
        self.assertEqual(ops.get("StoreAttrCached", 0), 0)

        counter.increment()
        self.assertEqual(counter.value, 6)

    def test_slot_read_only_access_lowers_to_field_ops(self) -> None:
        class Point:
            __slots__ = ("x", "y")

            def norm(self) -> int:
                return self.x * self.x + self.y * self.y

        point = Point()
        point.x = 3
        point.y = 4

        specialize(Point.norm, point.norm)

        names = opnames(Point.norm)
        self.assertIn("LOAD_ATTR_SLOT", names)
        self.assertTrue(cinderx.jit.is_jit_compiled(Point.norm))

        ops = cinderx.jit.get_function_hir_opcode_counts(Point.norm)
        self.assertGreaterEqual(ops.get("LoadField", 0), 2)
        self.assertGreaterEqual(ops.get("LoadAttr", 0), 2)
        self.assertGreaterEqual(ops.get("PrimitiveCompare", 0), 2)
        self.assertGreaterEqual(ops.get("CondBranch", 0), 2)
        self.assertGreaterEqual(ops.get("CheckField", 0), 2)
        self.assertEqual(ops.get("LoadAttrCached", 0), 0)
        self.assertEqual(point.norm(), 25)

    def test_dynamic_attr_read_does_not_use_slot_field_path(self) -> None:
        class DynamicPoint:
            def __init__(self, x: int, y: int) -> None:
                self.x = x
                self.y = y

            def norm(self) -> int:
                return self.x * self.x + self.y * self.y

        point = DynamicPoint(3, 4)

        specialize(DynamicPoint.norm, point.norm)

        names = opnames(DynamicPoint.norm)
        self.assertNotIn("LOAD_ATTR_SLOT", names)
        self.assertTrue(cinderx.jit.is_jit_compiled(DynamicPoint.norm))

        self.assertEqual(point.norm(), 25)

        point.x = 6
        point.y = 8
        self.assertEqual(point.norm(), 100)

    def test_slot_specialized_load_method_pushes_null(self) -> None:
        def target() -> int:
            return 42

        class CallableSlot:
            __slots__ = ("fn",)

            def call(self) -> int:
                return self.fn()

        obj = CallableSlot()
        obj.fn = target

        specialize(CallableSlot.call, obj.call)

        self.assertIn("LOAD_ATTR_SLOT", opnames(CallableSlot.call))
        self.assertTrue(cinderx.jit.is_jit_compiled(CallableSlot.call))
        ops = cinderx.jit.get_function_hir_opcode_counts(CallableSlot.call)
        self.assertGreaterEqual(ops.get("LoadField", 0), 1)
        self.assertGreaterEqual(ops.get("CheckField", 0), 1)
        self.assertEqual(ops.get("LoadAttrCached", 0), 0)
        self.assertEqual(obj.call(), 42)

        del obj.fn
        with self.assertRaises(AttributeError):
            obj.call()

        obj.fn = 42
        with self.assertRaises(TypeError):
            obj.call()

    def test_slot_unset_matches_interpreter_exception(self) -> None:
        class Counter:
            __slots__ = ("value",)

            def read(self) -> object:
                return self.value

        obj = Counter()
        obj.value = 1
        specialize(Counter.read, obj.read)

        del obj.value
        with self.assertRaises(AttributeError):
            obj.read()

    def test_slot_type_version_change_deopts(self) -> None:
        class Counter:
            __slots__ = ("value",)

            def read(self) -> object:
                return self.value

        obj = Counter()
        obj.value = 1
        specialize(Counter.read, obj.read)

        Counter.value = property(lambda self: 99)
        self.assertEqual(obj.read(), 99)

    def test_slot_type_version_change_deopts_store(self) -> None:
        events: list[object] = []

        class Counter:
            __slots__ = ("value",)

            def write(self, value: object) -> str:
                self.value = value
                return "done"

        obj = Counter()
        obj.value = 1
        specialize(Counter.write, lambda: obj.write(2))

        Counter.value = property(
            lambda self: 99, lambda self, value: events.append(value)
        )
        self.assertEqual(obj.write(42), "done")
        self.assertEqual(events, [42])

    def test_slot_subclass_receiver_load_falls_back_without_deopt(self) -> None:
        class Base:
            __slots__ = ("value",)

            def read(self) -> object:
                return self.value

        class Sub(Base):
            pass

        base = Base()
        base.value = 1
        sub = Sub()
        sub.value = 2

        specialize(Base.read, lambda: base.read())
        self.assertIn("LOAD_ATTR_SLOT", opnames(Base.read))

        cinderx.jit.clear_runtime_stats()
        self.assertEqual(Base.read(sub), 2)
        self.assert_deopt_count(0)

    def test_slot_subclass_unset_load_fallback_exception_handler(self) -> None:
        class Base:
            __slots__ = ("fallback", "value")

            def read_or_fallback(self) -> tuple[object, int]:
                try:
                    return self.value, -1
                except AttributeError as exc:
                    return self.fallback, exc.__traceback__.tb_lineno

        class Sub(Base):
            pass

        base = Base()
        base.value = 1
        base.fallback = 0
        sub = Sub()
        sub.fallback = 2

        specialize(Base.read_or_fallback, lambda: base.read_or_fallback())
        self.assertIn("LOAD_ATTR_SLOT", opnames(Base.read_or_fallback))

        expected_lineno = Base.read_or_fallback.__code__.co_firstlineno + 2
        cinderx.jit.clear_runtime_stats()
        self.assertEqual(Base.read_or_fallback(sub), (2, expected_lineno))
        self.assert_single_deopt(
            "UnhandledException", "LoadAttr", expected_lineno
        )

    def test_slot_subclass_fallback_load_refcount_is_balanced(self) -> None:
        class Payload:
            pass

        class Base:
            __slots__ = ("value",)

            def read(self) -> object:
                return self.value

        class Sub(Base):
            pass

        base = Base()
        base.value = Payload()
        payload = Payload()
        sub = Sub()
        sub.value = payload

        specialize(Base.read, lambda: base.read())
        self.assertIn("LOAD_ATTR_SLOT", opnames(Base.read))

        cinderx.jit.clear_runtime_stats()
        refcount = sys.getrefcount(payload)
        for _ in range(100):
            result = Base.read(sub)
            self.assertIs(result, payload)
            del result
        self.assertEqual(sys.getrefcount(payload), refcount)
        self.assert_deopt_count(0)

    def test_slot_subclass_receiver_deopts(self) -> None:
        class Base:
            __slots__ = ("value",)

            def read(self) -> object:
                return self.value

            def write(self, value: object) -> None:
                self.value = value

        class Sub(Base):
            pass

        base = Base()
        base.value = 1
        sub = Sub()
        sub.value = 2

        specialize(Base.read, lambda: base.read())
        self.assertIn("LOAD_ATTR_SLOT", opnames(Base.read))
        self.assertEqual(Base.read(sub), 2)

        specialize(Base.write, lambda: base.write(3))
        self.assertIn("STORE_ATTR_SLOT", opnames(Base.write))
        Base.write(sub, 4)
        self.assertEqual(sub.value, 4)

    def test_slot_specialized_disabled_matches_enabled(self) -> None:
        class Counter:
            __slots__ = ("value",)

            def increment(self) -> None:
                self.value = self.value + 1

        def run_with_lowering(enabled: bool) -> int:
            counter = Counter()
            counter.value = 0
            if enabled:
                cinderx.jit.enable_specialized_opcodes()
            else:
                cinderx.jit.disable_specialized_opcodes()
            specialize(Counter.increment, counter.increment)
            counter.increment()
            return counter.value

        try:
            self.assertEqual(run_with_lowering(False), run_with_lowering(True))
        finally:
            cinderx.jit.enable_specialized_opcodes()

    def test_slot_jit_matches_interpreter(self) -> None:
        class Counter:
            __slots__ = ("value",)

            def increment(self) -> None:
                self.value = self.value + 1

        def run(jit_enabled: bool) -> int:
            counter = Counter()
            counter.value = 0
            cinderx.jit.force_uncompile(Counter.increment)
            if jit_enabled:
                specialize(Counter.increment, counter.increment)
                counter.value = 0
            else:
                cinderx.jit.jit_suppress(Counter.increment)
            try:
                for _ in range(6):
                    counter.increment()
            finally:
                cinderx.jit.jit_unsuppress(Counter.increment)
            return counter.value

        self.assertEqual(run(False), run(True))

    def test_slot_store_decrefs_previous_value_once(self) -> None:
        events: list[str] = []

        class Watched:
            def __init__(self, name: str) -> None:
                self.name = name

            def __del__(self) -> None:
                events.append(self.name)

        class Holder:
            __slots__ = ("value",)

            def replace(self, value: object) -> None:
                self.value = value

        holder = Holder()
        holder.value = Watched("warm")
        specialize(Holder.replace, lambda: holder.replace(Watched("warmup")))

        events.clear()
        holder.value = Watched("old")
        events.clear()
        holder.replace(Watched("new"))
        self.assertEqual(events, ["old"])


if __name__ == "__main__":
    unittest.main()
