# Copyright (c) Meta Platforms, Inc. and affiliates.

import platform
import unittest

import cinderx.jit
from cinderx.jit import force_compile, force_uncompile, is_jit_compiled


# ---------------------------------------------------------------------------
# S1 / E1 — loop-carried numeric value across helper call
# ---------------------------------------------------------------------------
#
# S1: 编写并执行循环数值累加函数，循环体包含 helper_func(x) 调用，
#     核心变量 acc 跨调用点保活，并与 scale、bias 共同参与累加计算。
# E1: JIT 编译后计算结果与解释执行一致，运行期不产生非预期 deopt，
#     验证寄存器保留偏好不改变 Python 语义。


def _helper(x: int) -> int:
    return x + 1


def _loop_call_accumulate(
    xs: list[int], scale: int, bias: int, helper_func=_helper
) -> int:
    acc = 0
    for x in xs:
        acc += helper_func(x) * scale + bias
    return acc


class KunpengLoopCallCalleeSavedPrefTests(unittest.TestCase):
    def setUp(self) -> None:
        if platform.machine().lower() not in {"aarch64", "arm64"}:
            self.skipTest("ARM64 register allocation preference is target-specific")
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        force_uncompile(_loop_call_accumulate)

    def test_loop_call_accumulator_matches_interpreter_without_deopt(self) -> None:
        """S1/E1: loop + helper call + numeric accumulation remains stable."""
        xs = [1, 2, 3, 4]
        scale = 3
        bias = 5
        expected = sum((x + 1) * scale + bias for x in xs)

        self.assertEqual(_loop_call_accumulate(xs, scale, bias), expected)
        self.assertTrue(force_compile(_loop_call_accumulate))
        self.assertTrue(is_jit_compiled(_loop_call_accumulate))

        cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(_loop_call_accumulate(xs, scale, bias), expected)
        stats = cinderx.jit.get_and_clear_runtime_stats()
        deopt_qualnames = [
            d["normal"]["func_qualname"]
            for d in stats["deopt"]  # pyrefly: ignore [not-iterable]
        ]
        self.assertNotIn(_loop_call_accumulate.__qualname__, deopt_qualnames)
        self.assertEqual(stats["deopt"], [])


if __name__ == "__main__":
    unittest.main()
