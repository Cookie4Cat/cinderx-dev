# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Counter-backed proof that the CPython 3.11 capability gate holds.

The behavioural gate tests (test_jit_unsupported_311) pin that hot code is
never observed to run as machine code.  These tests pin the same fact from
the other side, through the monotonic trigger counters: under load and under
explicit compile requests, the JIT never allocates executable memory and
never creates a CompiledFunction.  This is the counter form of the source-set
round's acceptance items 6 and 7 (docs/cp311-jit-dev-plan.md, MR-02).
"""

import sys
import unittest

import _cinderx
from cinderx import jit


def hot(n: int) -> int:
    total = 0
    for i in range(n):
        total += i
    return total


class TriggerStatsGateTest(unittest.TestCase):
    def setUp(self) -> None:
        if sys.version_info[:2] != (3, 11):
            self.skipTest("the capability gate counters are pinned on 3.11")

    def test_stats_are_exported(self) -> None:
        stats = _cinderx._get_trigger_stats()
        self.assertEqual(
            sorted(stats),
            [
                "compiled_function_creations",
                "executable_alloc_bytes",
                "executable_alloc_calls",
                "machine_code_entries",
            ],
        )
        for key, value in stats.items():
            self.assertIsInstance(value, int, key)
            self.assertGreaterEqual(value, 0, key)

    def test_gate_holds_under_load_and_forced_compile(self) -> None:
        # Drive well past any plausible auto-JIT threshold, then ask for
        # compilation explicitly.
        for _ in range(2000):
            hot(50)
        self.assertFalse(jit.force_compile(hot))
        self.assertFalse(jit.is_jit_compiled(hot))

        stats = _cinderx._get_trigger_stats()
        self.assertEqual(stats["executable_alloc_calls"], 0)
        self.assertEqual(stats["executable_alloc_bytes"], 0)
        self.assertEqual(stats["compiled_function_creations"], 0)
        self.assertEqual(stats["machine_code_entries"], 0)

    def test_counters_never_decrease(self) -> None:
        before = _cinderx._get_trigger_stats()
        hot(1000)
        after = _cinderx._get_trigger_stats()
        for key in before:
            self.assertGreaterEqual(after[key], before[key], key)


if __name__ == "__main__":
    unittest.main()
