# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf


def _int_h(a: int) -> int:
    return a + 1


def _int_g(x: int, b: int, c: int) -> int:
    return x * 100 + b * 10 + c


def _int_call_result_chain(a: int, b: int, c: int) -> int:
    return _int_g(_int_h(a), b, c)


def _int_use(x: int) -> int:
    return x * 2


def _shared_intermediate_result(a: int) -> int:
    tmp = _int_h(a)
    side = _int_use(tmp)
    return _int_g(tmp, side, 7)


def _float_h(a: float) -> float:
    return a + 0.5


def _float_g(x: float, b: float) -> float:
    return x * 10.0 + b


def _float_call_result_chain(a: float, b: float) -> float:
    return _float_g(_float_h(a), b)


_JIT_FUNCS = (
    _int_h,
    _int_g,
    _int_call_result_chain,
    _int_use,
    _shared_intermediate_result,
    _float_h,
    _float_g,
    _float_call_result_chain,
)


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class Arm64CallResultMoveChainTests(unittest.TestCase):
    def setUp(self) -> None:
        self._force_uncompile()

    def tearDown(self) -> None:
        self._force_uncompile()

    def _force_uncompile(self) -> None:
        for func in _JIT_FUNCS:
            cinderx.jit.force_uncompile(func)

    def _force_compile(self, *funcs) -> None:
        for func in funcs:
            self.assertTrue(cinderx.jit.force_compile(func), func.__name__)
            self.assertTrue(cinderx.jit.is_jit_compiled(func), func.__name__)

    def test_int_call_result_chain_matches_interpreter(self) -> None:
        cases = ((2, 3, 4), (10, 20, 30), (-3, 5, 6))
        expected = [334, 1330, -144]

        self._force_compile(_int_h, _int_g, _int_call_result_chain)

        self.assertEqual(
            [_int_call_result_chain(*case) for case in cases],
            expected,
        )

    def test_shared_intermediate_result_stays_live(self) -> None:
        cases = (4, 8, -2)
        expected = [607, 1087, -113]

        self._force_compile(
            _int_h,
            _int_g,
            _int_use,
            _shared_intermediate_result,
        )

        self.assertEqual(
            [_shared_intermediate_result(case) for case in cases],
            expected,
        )

    def test_float_call_result_chain_matches_interpreter(self) -> None:
        cases = ((1.25, 3.0), (-2.5, 0.25), (8.0, -4.5))
        expected = [20.5, -19.75, 80.5]

        self._force_compile(_float_h, _float_g, _float_call_result_chain)

        for case, result in zip(cases, expected):
            self.assertAlmostEqual(_float_call_result_chain(*case), result)


if __name__ == "__main__":
    unittest.main()
