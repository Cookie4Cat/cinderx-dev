# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import unittest
from typing import Callable

import cinderx
import cinderx.jit
from cinderx.test_support import fail_if_deopt, passIf, skip_if_ft
from test_cinderx.test_jit_specialization import opnames

HIRCounts = dict[str, int]


def _specialize_load_attr_method_with_values(
    func: Callable[..., object], runner: Callable[[], object]
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(64):
            runner()

        names = opnames(func)
        if "LOAD_ATTR_METHOD_WITH_VALUES" not in names:
            raise AssertionError(names)

        cinderx.jit.jit_unsuppress(func)
        if not cinderx.jit.force_compile(func):
            raise AssertionError("force_compile failed")
    finally:
        cinderx.jit.jit_unsuppress(func)


@unittest.skipIf(
    sys.version_info < (3, 14),
    "LOAD_ATTR_METHOD_WITH_VALUES is only consumed on Python 3.14+",
)
@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@skip_if_ft("Inline caches disabled with free-threading")
class LoadAttrMethodWithValuesTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def assertHasSpecializedOpcode(
        self, func: Callable[..., object], opcode: str
    ) -> None:
        actual_opnames = opnames(func)
        self.assertIn(opcode, actual_opnames, actual_opnames)

    def finalHirCounts(self, func: Callable[..., object]) -> HIRCounts:
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        if counts is None:
            self.fail("function was not JIT compiled")
        return counts

    def assertMethodWithValuesHir(self, func: Callable[..., object]) -> None:
        counts = self.finalHirCounts(func)
        self.assertGreaterEqual(counts.get("CallStatic", 0), 1, counts)
        self.assertGreaterEqual(counts.get("GetSecondOutput", 0), 1, counts)
        self.assertEqual(counts.get("LoadAttr", 0), 0, counts)
        self.assertEqual(counts.get("LoadAttrCached", 0), 0, counts)
        self.assertEqual(counts.get("LoadMethod", 0), 0, counts)
        self.assertEqual(counts.get("LoadMethodCached", 0), 0, counts)

    def test_type_change_falls_back_to_new_type_method(self) -> None:
        class C:
            def method(self, value):
                return value + 10

        class D:
            def method(self, value):
                return value + 20

        def call(obj, value):
            return obj.method(value)

        c_obj = C()
        d_obj = D()
        _specialize_load_attr_method_with_values(call, lambda: call(c_obj, 2))
        checked = fail_if_deopt(call)

        self.assertEqual(checked(c_obj, 2), 12)
        self.assertEqual(checked(d_obj, 2), 22)
        self.assertMethodWithValuesHir(call)

    def test_same_type_multiple_instances_keep_layout_based_fast_path(
        self,
    ) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def call_both(first, second):
            return first.method(2), second.method(3)

        first = C(10)
        second = C(20)
        _specialize_load_attr_method_with_values(
            call_both, lambda: call_both(first, second)
        )
        checked = fail_if_deopt(call_both)

        self.assertEqual(checked(first, second), (12, 23))
        self.assertEqual(checked(C(30), C(40)), (32, 43))
        self.assertMethodWithValuesHir(call_both)

    def test_instance_attribute_shadowing_uses_instance_attribute(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def call(obj, value):
            return obj.method(value)

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj, 2))
        obj.method = lambda value: value + 100
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj, 2), 102)
        self.assertMethodWithValuesHir(call)

    def test_other_instance_shadowing_uses_shadowed_value(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self):
                return self.base

        def call(obj):
            return obj.method()

        first = C(1)
        second = C(2)
        _specialize_load_attr_method_with_values(call, lambda: call(first))
        second.method = lambda: 99
        checked = fail_if_deopt(call)

        self.assertEqual(checked(first), 1)
        self.assertEqual(checked(second), 99)
        self.assertMethodWithValuesHir(call)

    def test_unrelated_instance_layout_change_keeps_method_semantics(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self):
                return self.base

        def call(obj):
            return obj.method()

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj))
        obj.extra = "layout changed"
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj), 40)
        self.assertMethodWithValuesHir(call)

    def test_loop_method_call_keeps_specialized_method_path(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def loop(obj, limit):
            total = 0
            for i in range(limit):
                total += obj.method(i)
            return total

        obj = C(10)
        _specialize_load_attr_method_with_values(loop, lambda: loop(obj, 5))
        checked = fail_if_deopt(loop)

        self.assertEqual(checked(obj, 5), 60)
        self.assertHasSpecializedOpcode(loop, "LOAD_ATTR_METHOD_WITH_VALUES")
        self.assertMethodWithValuesHir(loop)

    def test_property_and_custom_descriptor_keep_descriptor_semantics(
        self,
    ) -> None:
        events = []

        class WithProperty:
            @property
            def method(self):
                events.append("property")
                return lambda: 99

        class Descriptor:
            def __get__(self, obj, owner):
                events.append(("descriptor", obj.base))
                return lambda: obj.base + 1

        class WithDescriptor:
            method = Descriptor()

            def __init__(self):
                self.base = 100

        class C:
            def method(self):
                return 42

        def call(obj):
            return obj.method()

        obj = C()
        _specialize_load_attr_method_with_values(call, lambda: call(obj))
        checked = fail_if_deopt(call)

        self.assertEqual(checked(WithProperty()), 99)
        self.assertEqual(checked(WithDescriptor()), 101)
        self.assertEqual(events, ["property", ("descriptor", 100)])
        self.assertMethodWithValuesHir(call)

    def test_same_type_method_replaced_with_property_keeps_semantics(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def call(obj, value):
            return obj.method(value)

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj, 2))

        C.method = property(lambda self: lambda value: self.base + value + 200)
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj, 2), 242)
        self.assertMethodWithValuesHir(call)

    def test_same_type_method_replaced_with_plain_callable_keeps_semantics(
        self,
    ) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        class CallableMethod:
            def __call__(self, value):
                return value + 300

        def call(obj, value):
            return obj.method(value)

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj, 2))

        C.method = CallableMethod()
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj, 2), 302)
        self.assertMethodWithValuesHir(call)

    def test_plain_instance_method_keeps_specialized_method_path(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def call(obj, value):
            return obj.method(value)

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj, 2))
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj, 2), 42)
        self.assertHasSpecializedOpcode(call, "LOAD_ATTR_METHOD_WITH_VALUES")
        self.assertMethodWithValuesHir(call)

    def test_class_method_replacement_falls_back_to_new_method(self) -> None:
        class C:
            def __init__(self, base):
                self.base = base

            def method(self, value):
                return self.base + value

        def call(obj, value):
            return obj.method(value)

        obj = C(40)
        _specialize_load_attr_method_with_values(call, lambda: call(obj, 2))

        def new_method(self, value):
            return self.base + value + 100

        C.method = new_method
        checked = fail_if_deopt(call)

        self.assertEqual(checked(obj, 2), 142)
        self.assertMethodWithValuesHir(call)

    def test_missing_method_preserves_attribute_error(self) -> None:
        class C:
            def method(self):
                return 42

        class Missing:
            pass

        def call(obj):
            return obj.method()

        obj = C()
        _specialize_load_attr_method_with_values(call, lambda: call(obj))

        with self.assertRaises(AttributeError):
            call(Missing())
        self.assertMethodWithValuesHir(call)

    def test_missing_method_getattr_callable_keeps_semantics(self) -> None:
        class C:
            def method(self):
                return 42

        class GetattrCallable:
            def __getattr__(self, name):
                if name == "method":
                    return lambda: 77
                raise AttributeError(name)

        def call(obj):
            return obj.method()

        obj = C()
        _specialize_load_attr_method_with_values(call, lambda: call(obj))
        checked = fail_if_deopt(call)

        self.assertEqual(checked(GetattrCallable()), 77)
        self.assertMethodWithValuesHir(call)


if __name__ == "__main__":
    unittest.main()
