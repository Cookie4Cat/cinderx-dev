import dis
import sys

import pytest

import cinderx.jit


_all_opnames = list(dis.opname)
if hasattr(dis, "_specialized_instructions"):
    _specialized_indices = [
        index for index, name in enumerate(_all_opnames) if name.startswith("<")
    ]

    for index, name in zip(_specialized_indices, dis._specialized_instructions):
        _all_opnames[index] = name


def _opnames(func):
    return [_all_opnames[insn.opcode] for insn in dis.Bytecode(func, adaptive=True)]


def _expected_subscr_opname(container):
    prefix = "BINARY_OP_SUBSCR" if sys.version_info >= (3, 14) else "BINARY_SUBSCR"
    return f"{prefix}_{container}"


def _specialize_then_compile(func, runner, expected_opname, expected_hir_opname):
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)

    try:
        for _ in range(20):
            runner()

        opnames = _opnames(func)
        assert expected_opname in opnames, opnames

        cinderx.jit.jit_unsuppress(func)
        assert cinderx.jit.force_compile(func)
        hir_counts = cinderx.jit.get_function_hir_opcode_counts(func)
        assert hir_counts is not None
        assert hir_counts.get(expected_hir_opname, 0) > 0, hir_counts
    finally:
        cinderx.jit.jit_unsuppress(func)


class _Index:
    def __init__(self, value):
        self.value = value

    def __index__(self):
        return self.value


class _BadIndex:
    def __index__(self):
        raise RuntimeError("index failed")


@pytest.fixture(autouse=True)
def _specialized_opcodes():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")

    cinderx.jit.enable_specialized_opcodes()


def test_binary_op_subscr_list_int_semantics_and_guard_misses():
    def f(container, index):
        return container[index]

    _specialize_then_compile(
        f,
        lambda: f(["a", "b"], 0),
        _expected_subscr_opname("LIST_INT"),
        "LoadArrayItem",
    )

    assert f(["a", "b"], -1) == "b"
    assert f(["a", "b"], False) == "a"
    assert f(["a", "b"], _Index(1)) == "b"
    assert f(["a", "b"], slice(0, 1)) == ["a"]
    with pytest.raises(RuntimeError, match="index failed"):
        f(["a", "b"], _BadIndex())

    class ListSubclass(list):
        def __getitem__(self, index):
            return ("override", index)

    assert f(ListSubclass(["a"]), 0) == ("override", 0)

    with pytest.raises(IndexError):
        f([], 0)
    with pytest.raises(IndexError):
        f([], -1)
    with pytest.raises(IndexError):
        f(["a"], 2)
    with pytest.raises(IndexError):
        f(["a"], -2)
    with pytest.raises(IndexError):
        f(["a"], 2**100)


def test_binary_op_subscr_tuple_int_semantics_and_guard_misses():
    def f(container, index):
        return container[index]

    _specialize_then_compile(
        f,
        lambda: f(("a", "b"), 0),
        _expected_subscr_opname("TUPLE_INT"),
        "LoadArrayItem",
    )

    assert f(("a", "b"), -1) == "b"
    assert f(("a", "b"), False) == "a"
    assert f(("a", "b"), _Index(1)) == "b"
    assert f(("a", "b"), slice(0, 1)) == ("a",)
    with pytest.raises(RuntimeError, match="index failed"):
        f(("a", "b"), _BadIndex())

    class TupleSubclass(tuple):
        def __getitem__(self, index):
            return ("override", index)

    assert f(TupleSubclass(("a",)), 0) == ("override", 0)

    with pytest.raises(IndexError):
        f((), 0)
    with pytest.raises(IndexError):
        f((), -1)
    with pytest.raises(IndexError):
        f(("a",), 2)
    with pytest.raises(IndexError):
        f(("a",), -2)
    with pytest.raises(IndexError):
        f(("a",), 2**100)


def test_binary_op_subscr_dict_semantics_and_guard_misses():
    def f(container, key):
        return container[key]

    _specialize_then_compile(
        f,
        lambda: f({"a": "b"}, "a"),
        _expected_subscr_opname("DICT"),
        "DictSubscr",
    )

    assert f({"x": "y"}, "x") == "y"

    class MissingDict(dict):
        def __missing__(self, key):
            return ("missing", key)

    class Mapping:
        def __getitem__(self, key):
            return ("mapped", key)

    class BadHash:
        def __hash__(self):
            raise RuntimeError("hash failed")

    assert f(MissingDict(), "x") == ("missing", "x")
    assert f(Mapping(), "x") == ("mapped", "x")

    with pytest.raises(KeyError) as excinfo:
        f({}, "x")
    assert excinfo.value.args == ("x",)
    with pytest.raises(TypeError):
        f({}, [])
    with pytest.raises(RuntimeError, match="hash failed"):
        f({}, BadHash())
