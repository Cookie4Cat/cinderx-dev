import sys
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf


def flip_prefix(perm, k):
    perm[: k + 1] = perm[k::-1]
    return perm


def flip_window(perm, k):
    perm[1 : k + 1] = perm[k::-1]
    return perm


def flip_stride(perm, k):
    perm[: k + 1] = perm[k::2]
    return perm


def caught_flip(perm, k):
    try:
        perm[: k + 1] = perm[k::-1]
    except ValueError:
        return "caught"
    return perm


def _force_compile(func):
    cinderx.jit.force_uncompile(func)
    assert cinderx.jit.force_compile(func)


@passIf(not cinderx.jit.is_enabled(), "requires CinderX JIT")
@unittest.skipIf(
    sys.version_info[:2] != (3, 14),
    "requires 3.14 slice bytecode",
)
class ListPrefixReverseAssignTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable()
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_lowers_to_runtime_fastpath(self) -> None:
        _force_compile(flip_prefix)
        counts = cinderx.jit.get_function_hir_opcode_counts(flip_prefix)

        self.assertGreaterEqual(counts.get("CallStatic", 0), 1, counts)
        self.assertEqual(counts.get("BuildSlice", 0), 0, counts)
        self.assertEqual(counts.get("StoreSubscr", 0), 0, counts)

    def test_non_prefix_and_stride_keep_generic_store_subscr(self) -> None:
        _force_compile(flip_window)
        _force_compile(flip_stride)

        self.assertEqual(flip_window([0, 1, 2, 3, 4], 3), [0, 3, 2, 1, 0, 4])
        self.assertEqual(flip_stride([0, 1, 2, 3, 4], 3), [3, 4])

        window_counts = cinderx.jit.get_function_hir_opcode_counts(flip_window)
        stride_counts = cinderx.jit.get_function_hir_opcode_counts(flip_stride)
        self.assertGreaterEqual(window_counts.get("StoreSubscr", 0), 1, window_counts)
        self.assertGreaterEqual(stride_counts.get("StoreSubscr", 0), 1, stride_counts)

    def test_prefix_reverse_assign_matches_python_semantics(self) -> None:
        _force_compile(flip_prefix)

        self.assertEqual(flip_prefix([0, 1, 2, 3, 4], 3), [3, 2, 1, 0, 4])
        self.assertEqual(flip_prefix([0, 1, 2, 3, 4], 10), [4, 3, 2, 1, 0])
        self.assertEqual(
            flip_prefix([0, 1, 2, 3, 4], -1),
            [4, 3, 2, 1, 0, 0, 1, 2, 3, 4],
        )

    def test_list_subclass_side_effects_are_preserved(self) -> None:
        _force_compile(flip_prefix)

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
        self.assertEqual(flip_prefix(value, 2), [2, 1, 0, 3])
        self.assertEqual((value.get_count, value.set_count), (1, 1))

    def test_compiled_exception_matches_interpreter(self) -> None:
        class Boom(list):
            def __setitem__(self, key, value):
                raise ValueError("boom")

        cinderx.jit.force_uncompile(flip_prefix)
        with self.assertRaises(ValueError):
            flip_prefix(Boom([0, 1, 2, 3]), 2)

        _force_compile(flip_prefix)
        with self.assertRaises(ValueError):
            flip_prefix(Boom([0, 1, 2, 3]), 2)

    def test_compiled_exception_caught_by_frame_handler(self) -> None:
        class Boom(list):
            def __setitem__(self, key, value):
                raise ValueError("boom")

        cinderx.jit.force_uncompile(caught_flip)
        self.assertEqual(caught_flip(Boom([0, 1, 2, 3]), 2), "caught")
        _force_compile(caught_flip)
        counts = cinderx.jit.get_function_hir_opcode_counts(caught_flip)
        self.assertGreaterEqual(counts.get("CallStatic", 0), 1, counts)

        self.assertEqual(caught_flip(Boom([0, 1, 2, 3]), 2), "caught")


if __name__ == "__main__":
    unittest.main()
