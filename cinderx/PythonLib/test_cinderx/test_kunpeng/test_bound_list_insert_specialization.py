import subprocess
import sys
import textwrap
import unittest

import cinderx.jit
from cinderx.test_support import subprocess_env


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class BoundListInsertSpecializationTests(unittest.TestCase):
    def setUp(self):
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self):
        cinderx.jit.disable_specialized_opcodes()

    def _compile_func(self, func, runner):
        cinderx.jit.force_uncompile(func)
        cinderx.jit.jit_suppress(func)
        try:
            for _ in range(20):
                runner()
            cinderx.jit.jit_unsuppress(func)
            self.assertTrue(cinderx.jit.force_compile(func))
            self.assertTrue(cinderx.jit.is_jit_compiled(func))
        finally:
            cinderx.jit.jit_unsuppress(func)

    def _exception_info(self, func, *args):
        try:
            func(*args)
        except Exception as exc:
            return type(exc), str(exc)
        return None, None

    def test_bound_list_insert_constant_index_skips_conversion_call(self):
        def insert_local(item):
            values = [1, 2, 3]
            insert = values.insert
            insert(0, item)
            return values

        self._compile_func(insert_local, lambda: insert_local(0))
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_local)
        self.assertEqual(counts.get("CallStatic", 0), 1)
        self.assertEqual(counts.get("VectorCall", 0), 0)
        self.assertEqual(insert_local("x"), ["x", 1, 2, 3])

    def test_bound_list_insert_dynamic_index_keeps_conversion_call(self):
        def insert_dynamic(values, index, item):
            insert = values.insert
            insert(index, item)

        values = [1, 2, 3]
        self._compile_func(
            insert_dynamic, lambda: insert_dynamic(values, 0, values.pop(0))
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_dynamic)
        if counts.get("CallStatic", 0):
            self.assertEqual(counts.get("CallStatic", 0), 2)
            self.assertEqual(counts.get("VectorCall", 0), 0)
        else:
            # A dynamic argument may remain untyped in HIR.  In that case the
            # exact-int prerequisite is not proven, so keeping VectorCall is
            # the required safe fallback.
            self.assertGreaterEqual(counts.get("VectorCall", 0), 1)

        insert_dynamic(values, 1, "x")
        self.assertEqual(values, [1, "x", 2, 3])

    def test_bound_list_insert_negative_index(self):
        def insert_negative(item):
            values = [1, 2, 3]
            insert = values.insert
            insert(-1, item)
            return values

        self._compile_func(insert_negative, lambda: insert_negative(0))
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_negative)
        self.assertEqual(counts.get("CallStatic", 0), 1)
        self.assertEqual(counts.get("VectorCall", 0), 0)
        self.assertEqual(insert_negative("x"), [1, 2, "x", 3])

    def test_bound_list_insert_negative_index_parallel_precompile(self):
        code = textwrap.dedent(
            """
            import cinderx.jit

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
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", code],
            capture_output=True,
            text=True,
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_bound_list_insert_preserves_receiver_and_value_refcounts(self):
        def insert_local(values, item):
            insert = values.insert
            insert(0, item)

        values = []

        class Payload:
            pass

        payload = Payload()

        def run_once():
            insert_local(values, payload)
            self.assertIs(values.pop(0), payload)

        self._compile_func(insert_local, run_once)
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_local)
        if counts.get("CallStatic", 0):
            self.assertEqual(counts.get("CallStatic", 0), 1)
            self.assertEqual(counts.get("VectorCall", 0), 0)
        else:
            self.assertGreaterEqual(counts.get("VectorCall", 0), 1)

        receiver_refs = sys.getrefcount(values)
        value_refs = sys.getrefcount(payload)
        for _ in range(200):
            run_once()
        self.assertEqual(sys.getrefcount(values), receiver_refs)
        self.assertEqual(sys.getrefcount(payload), value_refs)

    def test_bool_and_int_subclass_indexes_fall_back(self):
        def insert_bool(item):
            values = [1, 2]
            insert = values.insert
            insert(True, item)
            return values

        self._compile_func(insert_bool, lambda: insert_bool(0))
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_bool)
        self.assertGreaterEqual(counts.get("VectorCall", 0), 1)
        self.assertEqual(insert_bool("x"), [1, "x", 2])

        class IntSubclass(int):
            pass

        def insert_int_subclass(index, item):
            values = [1, 2]
            insert = values.insert
            insert(index, item)
            return values

        index = IntSubclass(0)
        self._compile_func(
            insert_int_subclass, lambda: insert_int_subclass(index, 0)
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_int_subclass)
        self.assertGreaterEqual(counts.get("VectorCall", 0), 1)
        self.assertEqual(insert_int_subclass(index, "x"), ["x", 1, 2])

    def test_non_exact_list_receiver_falls_back(self):
        class ListSubclass(list):
            def __init__(self):
                super().__init__([1, 2])
                self.insert_calls = []

            def insert(self, index, item):
                self.insert_calls.append((index, item))
                return "overridden"

        def insert_subclass(values, index, item):
            insert = values.insert
            return insert(index, item)

        values = ListSubclass()
        self._compile_func(
            insert_subclass, lambda: insert_subclass(values, 0, "warmup")
        )
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_subclass)
        self.assertGreaterEqual(counts.get("VectorCall", 0), 1)
        self.assertEqual(insert_subclass(values, 1, "x"), "overridden")
        self.assertEqual(values.insert_calls[-1], (1, "x"))
        self.assertEqual(values, [1, 2])

    def test_bound_list_insert_overflow_matches_python(self):
        def generic_insert_huge(item):
            values = []
            values.insert(10**100, item)
            return values

        def insert_huge(item):
            values = []
            insert = values.insert
            insert(10**100, item)
            return values

        cinderx.jit.jit_suppress(generic_insert_huge)
        try:
            expected = self._exception_info(generic_insert_huge, 1)
        finally:
            cinderx.jit.jit_unsuppress(generic_insert_huge)

        self._compile_func(insert_huge, lambda: self._exception_info(insert_huge, 1))
        counts = cinderx.jit.get_function_hir_opcode_counts(insert_huge)
        self.assertEqual(counts.get("CallStatic", 0), 2)
        self.assertEqual(counts.get("VectorCall", 0), 0)
        self.assertEqual(self._exception_info(insert_huge, 1), expected)


if __name__ == "__main__":
    unittest.main()
