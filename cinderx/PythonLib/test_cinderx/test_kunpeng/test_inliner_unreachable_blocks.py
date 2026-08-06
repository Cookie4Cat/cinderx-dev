# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Regression test for https://gitcode.com/openeuler/cinderx/issues/15.

Inlining a short-circuit helper whose gates are constant-cached globals lets
Simplify fold the guarded CondBranches into Branches, leaving unreachable
blocks in the caller's CFG. CopyPropagation only rewrites uses in reachable
blocks, so Phis in those unreachable blocks kept referencing the outputs of
Assigns it deleted, and PhiElimination later segfaulted chasing the dangling
defs (chaseAssignOperand -> Instr::GetOperand on a freed instruction).
"""

import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf

gate_a = False
gate_b = False


def cold_path():
    return True


def short_circuit_condition():
    return (gate_a or gate_b) and cold_path()


def target():
    if short_circuit_condition():
        return 99
    return 42


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class InlinerUnreachableBlockTests(unittest.TestCase):
    def setUp(self) -> None:
        self._inliner_was_enabled = cinderx.jit.is_hir_inliner_enabled()
        cinderx.jit.enable_hir_inliner()

    def tearDown(self) -> None:
        if not self._inliner_was_enabled:
            cinderx.jit.disable_hir_inliner()

    def test_inlined_short_circuit_condition_compiles(self) -> None:
        funcs = (target, short_circuit_condition, cold_path)
        # test_jitlist.py runs earlier in the aggregate suite and leaves an
        # explicit JIT list active. Make the callee eligible for dependency
        # preloading so this test does not depend on process-wide test order.
        cinderx.jit.append_jit_list(
            f"{short_circuit_condition.__module__}:"
            f"{short_circuit_condition.__qualname__}"
        )
        for func in funcs:
            cinderx.jit.force_uncompile(func)
            cinderx.jit.jit_suppress(func)

        try:
            # Populate interpreter feedback while keeping cold_path() untaken.
            for _ in range(64):
                self.assertEqual(target(), 42)
        finally:
            for func in funcs:
                cinderx.jit.jit_unsuppress(func)

        # Compiling target() inlines short_circuit_condition() and used to
        # segfault in PhiElimination before unreachable blocks were removed.
        self.assertTrue(cinderx.jit.force_compile(target))
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        # The crash only reproduces when the helper is actually inlined; make
        # sure inlining was not silently skipped (e.g. missing preloader).
        self.assertGreaterEqual(
            cinderx.jit.get_num_inlined_functions(target), 1
        )
        self.assertEqual(target(), 42)


if __name__ == "__main__":
    unittest.main()
