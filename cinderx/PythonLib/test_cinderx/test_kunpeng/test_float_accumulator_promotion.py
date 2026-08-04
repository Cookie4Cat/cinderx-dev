import os
import unittest
from pathlib import Path

import cinderx.jit
from cinderx.test_support import run_python_child
from test_cinderx.test_kunpeng.child_cases.float_accumulator_promotion import (
    interpreted_result as _interpreted_result,
    make_accumulator as _make_accumulator,
    make_data_accumulator as _make_data_accumulator,
    make_mixed_accumulator as _make_mixed_accumulator,
    specialize_then_compile as _specialize_then_compile,
)


HELPER = (
    Path(__file__).with_name("child_cases")
    / "float_accumulator_promotion.py"
)


def _clean_env():
    env = os.environ.copy()
    for key in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "CINDERX_OSR_ENABLED",
        "CINDERX_PLUGIN_ENABLE",
        "PYTHONJITALL",
        "PYTHONJITAUTO",
        "PYTHONJITDEBUG",
        "PYTHONJITDISABLE",
        "PYTHONJITDUMPHIR",
        "PYTHONJITDUMPHIRPASSES",
        "PYTHONJITDUMPFINALHIR",
    ):
        env.pop(key, None)
    return env


def _run_hir_case(case):
    env = _clean_env()
    env.update(
        {
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITDUMPFINALHIR": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    completed = run_python_child(
        HELPER,
        case,
        env=env,
        timeout=120,
    )
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise AssertionError(
            f"{case}: subprocess failed with {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    if "Traceback" in output:
        raise AssertionError(output)
    return output


class FloatAccumulatorPromotionTests(unittest.TestCase):
    def setUp(self):
        if not cinderx.jit.is_enabled():
            self.skipTest("requires CinderX JIT")
        cinderx.jit.enable_specialized_opcodes()

    def assertMixedAccumulatorWasNotPromoted(self):
        hir = _run_hir_case("mixed")
        self.assertIn("CASE_RESULT mixed_accumulator compiled", hir)
        self.assertIn("DoubleBinaryOp<Add>", hir)
        self.assertIn("GuardType<FloatExact>", hir)
        self.assertNotIn("LoadConst<MortalFloatExact[0]>", hir)

    def test_float_accumulator_promotes_zero_entry_without_deopt(self):
        func = _make_accumulator()
        _specialize_then_compile(func)

        cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(func(1000), 1000.0)

        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(stats["deopt"], [])

        opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(opcodes)
        self.assertEqual(opcodes.get("DoubleBinaryOp"), 1)

    def test_float_accumulator_empty_loop_still_returns_int_zero(self):
        func = _make_accumulator()
        _specialize_then_compile(func)

        result = func(0)
        self.assertEqual(result, 0)
        self.assertIs(type(result), int)

    def test_float_accumulator_repeated_calls_remain_deopt_free(self):
        func = _make_accumulator()
        _specialize_then_compile(func)

        cinderx.jit.get_and_clear_runtime_stats()
        expected = [10.0, 100.0, 1000.0]
        observed = [func(10), func(100), func(1000)]
        self.assertEqual(observed, expected)

        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(stats["deopt"], [])

    def test_data_float_accumulator_matches_interpreter_without_repeated_deopt(
        self,
    ):
        func = _make_data_accumulator()
        data = [1.0] * 1000
        _specialize_then_compile(func, data)

        expected = _interpreted_result(_make_data_accumulator, data)
        cinderx.jit.get_and_clear_runtime_stats()
        for _ in range(3):
            self.assertEqual(func(data), expected)

        stats = cinderx.jit.get_and_clear_runtime_stats()
        self.assertEqual(stats["deopt"], [])

        opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(opcodes)
        self.assertGreaterEqual(opcodes.get("DoubleBinaryOp", 0), 1)

    def test_data_float_accumulator_empty_data_returns_int_zero(self):
        func = _make_data_accumulator()
        _specialize_then_compile(func, [1.0] * 1000)

        expected = _interpreted_result(_make_data_accumulator, [])
        result = func([])
        self.assertEqual(result, expected)
        self.assertIs(type(result), int)

        opcodes = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(opcodes)
        self.assertGreaterEqual(opcodes.get("DoubleBinaryOp", 0), 1)

    def test_mixed_accumulator_initial_argument_keeps_generic_path(self):
        func = _make_mixed_accumulator()
        warmup_data = [1.0]
        warmup_calls = [(warmup_data, 1), (warmup_data, 0.5)] * 10
        _specialize_then_compile(func, warmup_calls=warmup_calls)
        self.assertMixedAccumulatorWasNotPromoted()

        for data, initial in (
            ([1.0] * 1000, 1),
            ([1.0] * 1000, 0.5),
            ([], 1),
            ([], 0.5),
        ):
            expected = _interpreted_result(_make_mixed_accumulator, data, initial)
            result = func(data, initial)
            self.assertEqual(result, expected)
            self.assertIs(type(result), type(expected))


if __name__ == "__main__":
    unittest.main()
