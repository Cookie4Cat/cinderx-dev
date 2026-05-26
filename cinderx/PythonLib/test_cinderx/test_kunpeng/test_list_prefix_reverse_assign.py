import sys

import pytest

import cinderx.jit


def flip_prefix(perm, k):
    perm[: k + 1] = perm[k::-1]
    return perm


def flip_window(perm, k):
    perm[1 : k + 1] = perm[k::-1]
    return perm


def flip_stride(perm, k):
    perm[: k + 1] = perm[k::2]
    return perm


def _force_compile(func):
    cinderx.jit.force_uncompile(func)
    assert cinderx.jit.force_compile(func)


@pytest.fixture(autouse=True)
def _require_jit():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")
    cinderx.jit.enable()
    cinderx.jit.enable_specialized_opcodes()


@pytest.mark.skipif(sys.version_info[:2] != (3, 14), reason="requires 3.14 slice bytecode")
def test_list_prefix_reverse_assign_lowers_to_runtime_fastpath():
    _force_compile(flip_prefix)
    counts = cinderx.jit.get_function_hir_opcode_counts(flip_prefix)

    assert counts.get("CallStatic", 0) >= 1, counts
    assert counts.get("BuildSlice", 0) == 0, counts
    assert counts.get("StoreSubscr", 0) == 0, counts


@pytest.mark.skipif(sys.version_info[:2] != (3, 14), reason="requires 3.14 slice bytecode")
def test_list_prefix_reverse_assign_semantics_and_misses():
    _force_compile(flip_prefix)
    _force_compile(flip_window)
    _force_compile(flip_stride)

    assert flip_prefix([0, 1, 2, 3, 4], 3) == [3, 2, 1, 0, 4]
    assert flip_prefix([0, 1, 2, 3, 4], 10) == [4, 3, 2, 1, 0]
    assert flip_prefix([0, 1, 2, 3, 4], -1) == [
        4,
        3,
        2,
        1,
        0,
        0,
        1,
        2,
        3,
        4,
    ]
    assert flip_window([0, 1, 2, 3, 4], 3) == [0, 3, 2, 1, 0, 4]
    assert flip_stride([0, 1, 2, 3, 4], 3) == [3, 4]

    window_counts = cinderx.jit.get_function_hir_opcode_counts(flip_window)
    stride_counts = cinderx.jit.get_function_hir_opcode_counts(flip_stride)
    assert window_counts.get("StoreSubscr", 0) >= 1, window_counts
    assert stride_counts.get("StoreSubscr", 0) >= 1, stride_counts

    class ListSubclass(list):
        def __init__(self, value):
            super().__init__(value)
            self.get_count = 0
            self.set_count = 0

        def __getitem__(self, key):
            self.get_count += 1
            return super().__getitem__(key)

        def __setitem__(self, key, value):
            self.set_count += 1
            return super().__setitem__(key, value)

    value = ListSubclass([0, 1, 2, 3])
    assert flip_prefix(value, 2) == [2, 1, 0, 3]
    assert (value.get_count, value.set_count) == (1, 1)
