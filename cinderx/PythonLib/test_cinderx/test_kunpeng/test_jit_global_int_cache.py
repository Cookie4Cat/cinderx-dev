import unittest

import cinderx.jit

from test_cinderx.common import failUnlessHasOpcodes


_jit_large_global_int = 257
_jit_cross_small_int_global = 0
_jit_bool_guard_global_int = 257
_jit_float_guard_global_int = 257
_jit_str_guard_global_int = 257
_jit_int_subclass_guard_global_int = 257


class _MyInt(int):
    pass


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_large_global_int_and_update(value):
    global _jit_large_global_int
    result = _jit_large_global_int
    _jit_large_global_int = value
    return result


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_cross_small_int_global_and_update(value):
    global _jit_cross_small_int_global
    result = _jit_cross_small_int_global
    _jit_cross_small_int_global = value
    return result


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_bool_guard_global_int_and_update(value):
    global _jit_bool_guard_global_int
    result = _jit_bool_guard_global_int
    _jit_bool_guard_global_int = value
    return result


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_float_guard_global_int_and_update(value):
    global _jit_float_guard_global_int
    result = _jit_float_guard_global_int
    _jit_float_guard_global_int = value
    return result


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_str_guard_global_int_and_update(value):
    global _jit_str_guard_global_int
    result = _jit_str_guard_global_int
    _jit_str_guard_global_int = value
    return result


@failUnlessHasOpcodes("LOAD_GLOBAL")
def _load_int_subclass_guard_global_int_and_update(value):
    global _jit_int_subclass_guard_global_int
    result = _jit_int_subclass_guard_global_int
    _jit_int_subclass_guard_global_int = value
    return result


class LoadGlobalIntCacheTests(unittest.TestCase):
    def assert_global_int_load_uses_exact_int_type_guard(self, func):
        self.assertTrue(cinderx.jit.force_compile(func))
        ops = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertEqual(ops.get("GuardType", 0), 1)
        self.assertEqual(ops.get("GuardIs", 0), 0)

    def assert_mutable_global_int_load_does_not_deopt(self, func, values):
        self.assert_global_int_load_uses_exact_int_type_guard(func)

        cinderx.jit.get_and_clear_runtime_stats()
        for old_value, new_value in zip(values, values[1:]):
            self.assertEqual(func(new_value), old_value)
        stats = cinderx.jit.get_and_clear_runtime_stats()
        relevant_deopts = [
            d
            for d in stats["deopt"]  # pyrefly: ignore [not-iterable]
            if d["normal"]["func_qualname"] == func.__qualname__
        ]
        self.assertEqual(relevant_deopts, [])

    def assert_global_int_load_deopts_when_rebound_to_non_exact_int(
        self, func, non_exact_value
    ):
        self.assert_global_int_load_uses_exact_int_type_guard(func)

        cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(func(non_exact_value), 257)
        self.assertIs(func(258), non_exact_value)
        stats = cinderx.jit.get_and_clear_runtime_stats()
        relevant_deopts = [
            d
            for d in stats["deopt"]  # pyrefly: ignore [not-iterable]
            if d["normal"]["func_qualname"] == func.__qualname__
        ]
        self.assertGreaterEqual(len(relevant_deopts), 1)

    def test_mutable_large_global_int_load_does_not_deopt(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_large_global_int
        _jit_large_global_int = 257
        self.assert_mutable_global_int_load_does_not_deopt(
            _load_large_global_int_and_update,
            [257, 258, 259, 260],
        )

    def test_mutable_global_int_load_crosses_small_int_cache_without_deopt(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_cross_small_int_global
        _jit_cross_small_int_global = 0
        self.assert_mutable_global_int_load_does_not_deopt(
            _load_cross_small_int_global_and_update,
            [0, 1, 2, 255, 256, 257, 258],
        )

    def test_global_int_load_deopts_when_rebound_to_bool(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_bool_guard_global_int
        _jit_bool_guard_global_int = 257
        self.assert_global_int_load_deopts_when_rebound_to_non_exact_int(
            _load_bool_guard_global_int_and_update, True
        )

    def test_global_int_load_deopts_when_rebound_to_float(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_float_guard_global_int
        _jit_float_guard_global_int = 257
        non_exact_value = 1.5
        self.assert_global_int_load_deopts_when_rebound_to_non_exact_int(
            _load_float_guard_global_int_and_update, non_exact_value
        )

    def test_global_int_load_deopts_when_rebound_to_str(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_str_guard_global_int
        _jit_str_guard_global_int = 257
        non_exact_value = "foo"
        self.assert_global_int_load_deopts_when_rebound_to_non_exact_int(
            _load_str_guard_global_int_and_update, non_exact_value
        )

    def test_global_int_load_deopts_when_rebound_to_int_subclass(self):
        if not cinderx.jit.is_enabled():
            return
        global _jit_int_subclass_guard_global_int
        _jit_int_subclass_guard_global_int = 257
        int_subclass = _MyInt(258)
        self.assert_global_int_load_deopts_when_rebound_to_non_exact_int(
            _load_int_subclass_guard_global_int_and_update, int_subclass
        )
