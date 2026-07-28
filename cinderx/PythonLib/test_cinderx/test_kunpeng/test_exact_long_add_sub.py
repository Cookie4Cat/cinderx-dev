import sys
import unittest
from typing import Any

import cinderx.jit


def _add(left: Any, right: Any) -> Any:
    return left + right


def _sub(left: Any, right: Any) -> Any:
    return left - right


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class ExactLongAddSubTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.disable_specialized_opcodes()
        for func in (_add, _sub):
            cinderx.jit.force_uncompile(func)
            self.assertTrue(cinderx.jit.force_compile(func))
            counts = cinderx.jit.get_function_hir_opcode_counts(func)
            self.assertGreater(counts.get("BinaryOp", 0), 0)

    def tearDown(self) -> None:
        for func in (_add, _sub):
            cinderx.jit.force_uncompile(func)
        cinderx.jit.disable_specialized_opcodes()

    def test_exact_long_arithmetic(self) -> None:
        pairs = (
            (0, 0),
            (123456789, 0x55AA55AA),
            (-123456789, 0x55AA55AA),
            ((1 << 521) + (1 << 257) + 17, -((1 << 389) + 31)),
        )
        for left, right in pairs:
            self.assertEqual(_add(left, right), left + right)
            self.assertEqual(_sub(left, right), left - right)

    def test_bool_subclasses_and_non_ints_use_fallback(self) -> None:
        self.assertIs(type(_add(True, True)), int)
        self.assertIs(type(_sub(True, False)), int)

        class Reflected(int):
            def __radd__(self, other: Any) -> Any:
                return ("radd", other, int(self))

            def __rsub__(self, other: Any) -> Any:
                return ("rsub", other, int(self))

        rhs = Reflected(5)
        self.assertEqual(_add(3, rhs), ("radd", 3, 5))
        self.assertEqual(_sub(3, rhs), ("rsub", 3, 5))

        class LeftOverride(int):
            def __add__(self, other: Any) -> Any:
                return ("left-add", int(self), other)

            def __sub__(self, other: Any) -> Any:
                return ("left-sub", int(self), other)

        self.assertEqual(_add(LeftOverride(9), 3), ("left-add", 9, 3))
        self.assertEqual(_sub(LeftOverride(9), 3), ("left-sub", 9, 3))

        self.assertEqual(_add("ab", "cd"), "abcd")
        self.assertEqual(_add([1, 2], [3]), [1, 2, 3])

        class NonInt:
            def __add__(self, other: Any) -> Any:
                return ("non-int-add", other)

            def __sub__(self, other: Any) -> Any:
                return ("non-int-sub", other)

        value = NonInt()
        self.assertEqual(_add(value, 7), ("non-int-add", 7))
        self.assertEqual(_sub(value, 7), ("non-int-sub", 7))

    def test_fallback_exceptions_and_callsite_metadata(self) -> None:
        class MarkerError(Exception):
            pass

        class RaisingRight(int):
            def __radd__(self, other: Any) -> Any:
                raise MarkerError("reflected add marker")

            def __rsub__(self, other: Any) -> Any:
                raise MarkerError("reflected sub marker")

        for func, message in (
            (_add, "reflected add marker"),
            (_sub, "reflected sub marker"),
        ):
            with self.assertRaisesRegex(MarkerError, message):
                func(1, RaisingRight(2))

        trace_events: list[str] = []

        def traced_probe() -> int:
            return 42

        def tracer(frame: Any, event: str, arg: Any) -> Any:
            if frame.f_code is traced_probe.__code__:
                trace_events.append(event)
            return tracer

        class TraceEnabler(int):
            def __radd__(self, other: Any) -> Any:
                sys.settrace(tracer)
                return ("trace-enabled", other, int(self))

        try:
            self.assertEqual(
                _add(7, TraceEnabler(5)), ("trace-enabled", 7, 5)
            )
            self.assertEqual(traced_probe(), 42)
        finally:
            sys.settrace(None)
        self.assertIn("call", trace_events)
        self.assertIn("return", trace_events)

    def test_exact_and_fallback_reference_counts(self) -> None:
        left = (1 << 521) + 123
        right = (1 << 389) - 1
        left_refs = sys.getrefcount(left)
        right_refs = sys.getrefcount(right)
        for _ in range(2000):
            add_result = _add(left, right)
            sub_result = _sub(left, right)
        del add_result, sub_result
        self.assertEqual(sys.getrefcount(left), left_refs)
        self.assertEqual(sys.getrefcount(right), right_refs)

        class Reflected(int):
            def __radd__(self, other: Any) -> Any:
                return ("radd", other, int(self))

            def __rsub__(self, other: Any) -> Any:
                return ("rsub", other, int(self))

        fallback_left = int("1234567890123456789012345678901234567890")
        fallback_right = Reflected(5)
        fallback_left_refs = sys.getrefcount(fallback_left)
        fallback_right_refs = sys.getrefcount(fallback_right)
        for _ in range(2000):
            add_result = _add(fallback_left, fallback_right)
            sub_result = _sub(fallback_left, fallback_right)
        del add_result, sub_result
        self.assertEqual(sys.getrefcount(fallback_left), fallback_left_refs)
        self.assertEqual(sys.getrefcount(fallback_right), fallback_right_refs)


if __name__ == "__main__":
    unittest.main()
