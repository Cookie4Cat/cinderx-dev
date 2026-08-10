# Copyright (c) Meta Platforms, Inc. and affiliates.

"""CPython 3.11 custom interpreter loop and PEP 523 entry-point ownership."""

import sys
import unittest


def _specialization_target(value, offset=0):
    """Module-level callee with a default, reached via LOAD_GLOBAL.

    Both properties matter: the global lookup specializes to
    LOAD_GLOBAL_MODULE, and the defaulted signature to
    CALL_PY_WITH_DEFAULTS.
    """
    return value + offset


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Interpreter311Tests(unittest.TestCase):
    """Code executed while the CinderX evaluator is installed."""

    @classmethod
    def setUpClass(cls) -> None:
        import _cinderx
        import cinderx

        cinderx.init()
        cls._cinderx = _cinderx
        cls._installed_here = not _cinderx.is_frame_evaluator_installed()
        if cls._installed_here:
            _cinderx.install_frame_evaluator()

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._installed_here:
            cls._cinderx.remove_frame_evaluator()

    def test_evaluator_is_installed(self) -> None:
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

    def test_call_and_arguments(self) -> None:
        def add(a, b=2, *rest, kw=8, **kwargs):
            return a + b + sum(rest) + kw + sum(kwargs.values())

        self.assertEqual(add(1), 11)
        self.assertEqual(add(1, 2, 3, 4, kw=5, extra=6), 21)

    def test_exception_type_message_and_traceback(self) -> None:
        def fail():
            raise ValueError("formal-311-boom")

        frames = []
        try:
            fail()
        except ValueError as exc:
            self.assertEqual(str(exc), "formal-311-boom")
            traceback = exc.__traceback__
            while traceback is not None:
                frames.append(traceback.tb_frame.f_code.co_name)
                traceback = traceback.tb_next
        else:
            self.fail("fail() did not raise")

        # The traceback runs from the catching frame down to the raising one.
        self.assertEqual(frames[0], "test_exception_type_message_and_traceback")
        self.assertEqual(frames[-1], "fail")

    def test_exception_is_catchable_in_a_loop(self) -> None:
        # Exercises the exception table on a path that raises and recovers
        # many times, which is where a mis-wired evaluator shows up first.
        caught = 0
        for index in range(100):
            try:
                if index % 3:
                    raise KeyError(index)
            except KeyError:
                caught += 1
        self.assertEqual(caught, 66)

    def test_generator_protocol(self) -> None:
        def gen():
            received = yield 10
            yield received + 1

        instance = gen()
        self.assertEqual(next(instance), 10)
        self.assertEqual(instance.send(41), 42)
        with self.assertRaises(StopIteration):
            next(instance)

    def test_generator_close_and_throw(self) -> None:
        def gen():
            try:
                yield 1
            except RuntimeError:
                yield 2

        instance = gen()
        next(instance)
        self.assertEqual(instance.throw(RuntimeError("x")), 2)

        other = gen()
        next(other)
        other.close()

    def test_specialization_reaches_the_specialized_form(self) -> None:
        # The interpreter must still quicken and specialize under our
        # evaluator; the disassembly names the specialized opcode.
        import dis

        namespace = {"_specialization_target": _specialization_target}
        exec(
            "def loop():\n"
            "    for i in [1, 2, 3] * 3:\n"
            "        _specialization_target(i)\n",
            namespace,
        )
        loop = namespace["loop"]

        # Enough iterations for the call site to leave its adaptive backoff.
        for _ in range(30):
            loop()

        text = dis.Bytecode(loop, adaptive=True).dis()
        self.assertIn("LOAD_GLOBAL_MODULE", text)
        self.assertIn("CALL_PY_WITH_DEFAULTS", text)

    def test_frame_introspection(self) -> None:
        def inner():
            frame = sys._getframe()
            return frame.f_code.co_name, frame.f_back.f_code.co_name

        self.assertEqual(inner(), ("inner", "test_frame_introspection"))


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class EvalHookOwnership311Tests(unittest.TestCase):
    """Installing and removing the PEP 523 entry point."""

    def setUp(self) -> None:
        import _cinderx
        import cinderx

        cinderx.init()
        self._cinderx = _cinderx
        self._was_installed = _cinderx.is_frame_evaluator_installed()

    def tearDown(self) -> None:
        installed = self._cinderx.is_frame_evaluator_installed()
        if self._was_installed and not installed:
            self._cinderx.install_frame_evaluator()
        elif not self._was_installed and installed:
            self._cinderx.remove_frame_evaluator()

    def test_install_remove_round_trip(self) -> None:
        if self._was_installed:
            self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())

        self._cinderx.install_frame_evaluator()
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

        # Code keeps running correctly on either side of the switch.
        self.assertEqual(sum(i * i for i in range(10)), 285)

        self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())
        self.assertEqual(sum(i * i for i in range(10)), 285)

    def test_install_is_idempotent(self) -> None:
        self._cinderx.install_frame_evaluator()
        self._cinderx.install_frame_evaluator()
        self.assertTrue(self._cinderx.is_frame_evaluator_installed())

    def test_remove_is_idempotent(self) -> None:
        self._cinderx.install_frame_evaluator()
        self._cinderx.remove_frame_evaluator()
        self._cinderx.remove_frame_evaluator()
        self.assertFalse(self._cinderx.is_frame_evaluator_installed())


if __name__ == "__main__":
    unittest.main()
