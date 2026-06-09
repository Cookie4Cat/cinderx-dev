# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

import cinderx.jit
from test_cinderx.test_jit_specialization import opnames


class LoadMethodGetattrFallbackTests(unittest.TestCase):
    def assert_load_attr_slot_fallback_shape(self, func):
        self.assertIn("LOAD_ATTR_SLOT", opnames(func))
        if not cinderx.jit.is_jit_compiled(func):
            self.assertTrue(cinderx.jit.force_compile(func))
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertGreaterEqual(counts.get("LoadField", 0), 1, counts)
        self.assertGreaterEqual(
            counts.get("LoadAttr", 0) + counts.get("LoadAttrCached", 0),
            1,
            counts,
        )

    def test_empty_slot_attribute_error_uses_getattr_hook(self) -> None:
        class EmptyListener:
            def __call__(self, value):
                return ("called", value)

        class Dispatch:
            __slots__ = ("hook",)

            def __getattr__(self, name):
                if name != "hook":
                    raise AttributeError(name)
                listener = EmptyListener()
                self.hook = listener
                return listener

        def call_hook(dispatch):
            return dispatch.hook("value")

        dispatch = Dispatch()
        cinderx.jit.enable_specialized_opcodes()
        for _ in range(100):
            self.assertEqual(call_hook(dispatch), ("called", "value"))
        self.assert_load_attr_slot_fallback_shape(call_hook)
        self.assertEqual(call_hook(dispatch), ("called", "value"))
        self.assertIsInstance(dispatch.hook, EmptyListener)
        self.assertEqual(call_hook(dispatch), ("called", "value"))

    def test_empty_slot_getattr_attribute_error_is_catchable(self) -> None:
        class Dispatch:
            __slots__ = ("hook",)

            def __getattr__(self, name):
                raise AttributeError(name)

        def call_missing(dispatch):
            return dispatch.hook

        def catch_missing(dispatch):
            try:
                call_missing(dispatch)
            except AttributeError as exc:
                return ("caught", exc.args)
            return ("missing", ())

        dispatch = Dispatch()
        cinderx.jit.enable_specialized_opcodes()
        for _ in range(100):
            self.assertEqual(catch_missing(dispatch), ("caught", ("hook",)))
        self.assert_load_attr_slot_fallback_shape(call_missing)
        self.assertEqual(catch_missing(dispatch), ("caught", ("hook",)))

    def test_set_slot_uses_fast_path_without_getattr(self) -> None:
        class Dispatch:
            __slots__ = ("getattr_calls", "hook")

            def __init__(self):
                self.getattr_calls = 0
                self.hook = "ready"

            def __getattr__(self, name):
                self.getattr_calls += 1
                raise AttributeError(name)

        def read_hook(dispatch):
            return dispatch.hook

        dispatch = Dispatch()
        cinderx.jit.enable_specialized_opcodes()
        for _ in range(100):
            self.assertEqual(read_hook(dispatch), "ready")
        self.assert_load_attr_slot_fallback_shape(read_hook)
        self.assertEqual(read_hook(dispatch), "ready")
        self.assertEqual(dispatch.getattr_calls, 0)


if __name__ == "__main__":
    unittest.main()
