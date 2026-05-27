import dis
import sys
import unittest
from typing import Any, Callable, TypeVar

import cinderx.jit


TCallableRet = TypeVar("TCallableRet")


_ALL_OPNAMES = list(dis.opname)
if hasattr(dis, "_specialized_instructions"):
    _SPECIALIZED_INDICES = [
        index for index, name in enumerate(_ALL_OPNAMES) if name.startswith("<")
    ]
    for index, name in zip(_SPECIALIZED_INDICES, dis._specialized_instructions):
        _ALL_OPNAMES[index] = name


def opnames(func: Callable[..., TCallableRet]) -> list[str]:
    return [_ALL_OPNAMES[insn.opcode] for insn in dis.Bytecode(func, adaptive=True)]


def hir_opnames(func: Callable[..., TCallableRet]) -> set[str]:
    counts = cinderx.jit.get_function_hir_opcode_counts(func)
    assert counts is not None
    return set(counts)


def specialize(
    func: Callable[..., TCallableRet],
    callable: Callable[[], TCallableRet],
    warmup: int = 8,
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        for _ in range(warmup):
            callable()
    finally:
        cinderx.jit.jit_unsuppress(func)
    assert cinderx.jit.force_compile(func)


@unittest.skipUnless(sys.version_info >= (3, 14), "requires BINARY_OP_SUBSCR")
@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class BinaryOpSubscrSpecializationTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def assert_specialized_opcode(
        self, func: Callable[..., object], expected: str
    ) -> None:
        names = opnames(func)
        self.assertIn(expected, names)
        self.assertNotIn("BINARY_OP", names)

    def assert_hir_specialized_sequence_path(
        self, func: Callable[..., object]
    ) -> None:
        hir = hir_opnames(func)
        self.assertIn("LoadArrayItem", hir)
        self.assertNotIn("BinaryOp", hir)

    def assert_hir_generic_subscript_path(
        self, func: Callable[..., object]
    ) -> None:
        hir = hir_opnames(func)
        self.assertIn("BinaryOp", hir)
        self.assertNotIn("DictSubscr", hir)
        self.assertNotIn("LoadArrayItem", hir)

    def test_tuple_int_subscr_keeps_specialized_hir_path(self) -> None:
        def f(xs: tuple[str, str, str], i: int) -> str:
            return xs[i]

        specialize(f, lambda: f(("a", "b", "c"), 1))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_TUPLE_INT")
        self.assert_hir_specialized_sequence_path(f)
        self.assertEqual(f(("x", "y", "z"), 2), "z")

    def test_list_int_subscr_keeps_specialized_hir_path(self) -> None:
        def f(xs: list[str], i: int) -> str:
            return xs[i]

        specialize(f, lambda: f(["a", "b", "c"], 1))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_LIST_INT")
        self.assert_hir_specialized_sequence_path(f)
        self.assertEqual(f(["x", "y", "z"], 2), "z")

    def test_dict_missing_key_preserves_key_error(self) -> None:
        def f(d: dict[str, str], k: str) -> str:
            return d[k]

        specialize(f, lambda: f({"present": "value"}, "present"))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_DICT")
        self.assertIn("DictSubscr", hir_opnames(f))
        with self.assertRaises(KeyError):
            f({"present": "value"}, "missing")

    def test_dict_subscr_keeps_specialized_hir_path(self) -> None:
        def f(d: dict[str, str], k: str) -> str:
            return d[k]

        specialize(f, lambda: f({"a": "b"}, "a"))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_DICT")
        hir = hir_opnames(f)
        self.assertIn("DictSubscr", hir)
        self.assertNotIn("BinaryOp", hir)
        self.assertEqual(f({"c": "d"}, "c"), "d")

    def test_custom_getitem_stays_on_generic_subscript_path(self) -> None:
        class CustomGetItem:
            def __init__(self) -> None:
                self.keys: list[str] = []

            def __getitem__(self, key: str) -> str:
                self.keys.append(key)
                return f"value:{key}"

        def f(obj: Any, key: Any) -> Any:
            return obj[key]

        obj = CustomGetItem()
        specialize(f, lambda: f(obj, "a"))

        names = opnames(f)
        self.assertNotIn("BINARY_OP_SUBSCR_DICT", names)
        self.assertNotIn("BINARY_OP_SUBSCR_LIST_INT", names)
        self.assertNotIn("BINARY_OP_SUBSCR_TUPLE_INT", names)
        self.assert_hir_generic_subscript_path(f)
        self.assertEqual(f(obj, "b"), "value:b")
        self.assertEqual(obj.keys[-1], "b")

    def test_custom_len_to_bool_stays_on_generic_truth_path(self) -> None:
        class CustomLen:
            def __init__(self, length: int) -> None:
                self.length = length
                self.calls = 0

            def __len__(self) -> int:
                self.calls += 1
                return self.length

        def f(obj: Any) -> str:
            if obj:
                return "truthy"
            return "falsey"

        truthy = CustomLen(3)
        specialize(f, lambda: f(truthy))

        names = opnames(f)
        self.assertNotIn("TO_BOOL_LIST", names)
        self.assertNotIn("TO_BOOL_STR", names)
        self.assertIn("IsTruthy", hir_opnames(f))

        falsey = CustomLen(0)
        self.assertEqual(f(falsey), "falsey")
        self.assertEqual(falsey.calls, 1)

    def test_sequence_non_int_subscr_stays_generic(self) -> None:
        def f(xs: Any, i: Any) -> Any:
            return xs[i]

        cinderx.jit.force_uncompile(f)
        cinderx.jit.jit_suppress(f)
        try:
            for _ in range(8):
                with self.assertRaises(TypeError):
                    f(["a", "b"], "0")
        finally:
            cinderx.jit.jit_unsuppress(f)
        assert cinderx.jit.force_compile(f)

        names = opnames(f)
        self.assertNotIn("BINARY_OP_SUBSCR_LIST_INT", names)
        self.assertNotIn("BINARY_OP_SUBSCR_TUPLE_INT", names)
        self.assert_hir_generic_subscript_path(f)

        with self.assertRaises(TypeError):
            f(["a", "b"], "0")
        with self.assertRaises(TypeError):
            f(("a", "b"), 1.0)

    def test_list_int_subscr_guard_misses_preserve_python_semantics(self) -> None:
        class Index:
            def __init__(self, value: int) -> None:
                self.value = value

            def __index__(self) -> int:
                return self.value

        class BadIndex:
            def __index__(self) -> int:
                raise RuntimeError("index failed")

        class ListSubclass(list):
            def __getitem__(self, index: object) -> tuple[str, object]:
                return ("override", index)

        def f(xs: Any, i: Any) -> Any:
            return xs[i]

        specialize(f, lambda: f(["a", "b"], 0))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_LIST_INT")
        self.assertEqual(f(["a", "b"], -1), "b")
        self.assertEqual(f(["a", "b"], False), "a")
        self.assertEqual(f(["a", "b"], Index(1)), "b")
        self.assertEqual(f(["a", "b"], slice(0, 1)), ["a"])
        self.assertEqual(f(ListSubclass(["a"]), 0), ("override", 0))
        with self.assertRaisesRegex(RuntimeError, "index failed"):
            f(["a", "b"], BadIndex())
        with self.assertRaises(IndexError):
            f([], 0)
        with self.assertRaises(IndexError):
            f([], -1)
        with self.assertRaises(IndexError):
            f(["a"], 2)
        with self.assertRaises(IndexError):
            f(["a"], -2)
        with self.assertRaises(IndexError):
            f(["a"], 2**100)

    def test_tuple_int_subscr_guard_misses_preserve_python_semantics(self) -> None:
        class Index:
            def __init__(self, value: int) -> None:
                self.value = value

            def __index__(self) -> int:
                return self.value

        class BadIndex:
            def __index__(self) -> int:
                raise RuntimeError("index failed")

        class TupleSubclass(tuple):
            def __getitem__(self, index: object) -> tuple[str, object]:
                return ("override", index)

        def f(xs: Any, i: Any) -> Any:
            return xs[i]

        specialize(f, lambda: f(("a", "b"), 0))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_TUPLE_INT")
        self.assertEqual(f(("a", "b"), -1), "b")
        self.assertEqual(f(("a", "b"), False), "a")
        self.assertEqual(f(("a", "b"), Index(1)), "b")
        self.assertEqual(f(("a", "b"), slice(0, 1)), ("a",))
        self.assertEqual(f(TupleSubclass(("a",)), 0), ("override", 0))
        with self.assertRaisesRegex(RuntimeError, "index failed"):
            f(("a", "b"), BadIndex())
        with self.assertRaises(IndexError):
            f((), 0)
        with self.assertRaises(IndexError):
            f((), -1)
        with self.assertRaises(IndexError):
            f(("a",), 2)
        with self.assertRaises(IndexError):
            f(("a",), -2)
        with self.assertRaises(IndexError):
            f(("a",), 2**100)

    def test_dict_subscr_guard_misses_preserve_python_semantics(self) -> None:
        class MissingDict(dict):
            def __missing__(self, key: object) -> tuple[str, object]:
                return ("missing", key)

        class Mapping:
            def __getitem__(self, key: object) -> tuple[str, object]:
                return ("mapped", key)

        class BadHash:
            def __hash__(self) -> int:
                raise RuntimeError("hash failed")

        def f(container: Any, key: Any) -> Any:
            return container[key]

        specialize(f, lambda: f({"a": "b"}, "a"))

        self.assert_specialized_opcode(f, "BINARY_OP_SUBSCR_DICT")
        self.assertEqual(f(MissingDict(), "x"), ("missing", "x"))
        self.assertEqual(f(Mapping(), "x"), ("mapped", "x"))
        with self.assertRaises(KeyError) as excinfo:
            f({}, "x")
        self.assertEqual(excinfo.exception.args, ("x",))
        with self.assertRaises(TypeError):
            f({}, [])
        with self.assertRaisesRegex(RuntimeError, "hash failed"):
            f({}, BadHash())


if __name__ == "__main__":
    unittest.main()
