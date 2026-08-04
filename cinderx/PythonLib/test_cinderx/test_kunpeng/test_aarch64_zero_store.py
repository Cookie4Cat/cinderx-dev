# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import platform
import unittest
from typing import Optional

import cinderx.jit
from cinderx.test_support import skip_unless_jit


@unittest.skipUnless(
    platform.machine().lower() in {"aarch64", "arm64"},
    "Tests the AArch64 JIT backend",
)
@skip_unless_jit("Tests AArch64 JIT frame initialization")
class AArch64ZeroStoreIntegrationTest(unittest.TestCase):
    def test_object_and_integer_zero_initialization_preserves_results(self) -> None:
        def target(mode: int, value: object) -> tuple[Optional[object], int]:
            result: Optional[object] = None
            status: int = 0
            try:
                if mode == 1:
                    result = value
                elif mode == 2:
                    raise ValueError("expected")
            except ValueError:
                status = 1
            return result, status

        if not cinderx.jit.is_jit_compiled(target):
            self.assertTrue(cinderx.jit.force_compile(target))
        self.assertTrue(cinderx.jit.is_jit_compiled(target))

        marker: object = object()
        self.assertEqual(target(0, marker), (None, 0))
        self.assertEqual(target(1, marker), (marker, 0))
        self.assertEqual(target(2, marker), (None, 1))


if __name__ == "__main__":
    unittest.main()
