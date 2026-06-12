import gc
import sys
import unittest
import weakref

import cinderx
import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.jit import _deopt_gen


def _run_to_completion(coro):
    try:
        coro.send(None)
    except StopIteration as caught:
        return caught.value
    raise AssertionError("coroutine did not complete synchronously")


@unittest.skipUnless(cinderx.is_lightweight_frames_enabled(), "LWF not compiled in")
class JitCoroutineRuntimeTests(unittest.TestCase):
    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_recursive_coroutine_executes_in_jit(self):
        async def fibonacci(n):
            if n <= 1:
                return n
            return await fibonacci(n - 1) + await fibonacci(n - 2)

        self.assertTrue(cinderx.jit.force_compile(fibonacci))
        self.assertTrue(cinderx.jit.is_jit_compiled(fibonacci))
        self.assertEqual(_run_to_completion(fibonacci(8)), 21)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_coroutine_getframe_materializes_jit_frame(self):
        async def inspect_frame():
            frame = sys._getframe(0)
            builtins = __builtins__
            expected_builtins = (
                builtins.__dict__ if hasattr(builtins, "__dict__") else builtins
            )
            return (
                frame.f_code is inspect_frame.__code__,
                frame.f_globals is globals(),
                frame.f_builtins is expected_builtins,
                isinstance(frame.f_lasti, int),
                isinstance(frame.f_lineno, int),
            )

        self.assertTrue(cinderx.jit.force_compile(inspect_frame))
        self.assertEqual(
            _run_to_completion(inspect_frame()),
            (True, True, True, True, True),
        )

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_coroutine_traceback_materializes_jit_frame(self):
        async def fail():
            raise ValueError("jit coroutine traceback")

        self.assertTrue(cinderx.jit.force_compile(fail))
        try:
            fail().send(None)
        except ValueError as caught:
            tb = caught.__traceback__
        else:
            self.fail("ValueError was not raised")

        frames = []
        while tb is not None:
            code = tb.tb_frame.f_code
            frames.append((code.co_name, code.co_filename))
            tb = tb.tb_next

        self.assertIn(("fail", __file__), frames)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_created_and_finished_coroutine_has_no_await_target(self):
        async def done():
            return 5

        self.assertTrue(cinderx.jit.force_compile(done))
        coro = done()
        self.assertIsNone(coro.cr_await)
        self.assertEqual(_run_to_completion(coro), 5)
        self.assertIsNone(coro.cr_await)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_completed_coroutine_weakref_uses_generic_dealloc(self):
        events = []

        async def done():
            return 42

        self.assertTrue(cinderx.jit.force_compile(done))
        coro = done()
        ref = weakref.ref(coro, lambda _: events.append("dead"))

        self.assertEqual(_run_to_completion(coro), 42)
        del coro
        gc.collect()

        self.assertIsNone(ref())
        self.assertEqual(events, ["dead"])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_completed_coroutine_origin_uses_generic_dealloc(self):
        old_depth = sys.get_coroutine_origin_tracking_depth()
        try:
            sys.set_coroutine_origin_tracking_depth(2)

            async def done():
                return 7

            self.assertTrue(cinderx.jit.force_compile(done))
            coro = done()
            self.assertIsNotNone(coro.cr_origin)
            self.assertEqual(_run_to_completion(coro), 7)
            del coro
            gc.collect()
        finally:
            sys.set_coroutine_origin_tracking_depth(old_depth)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_suspended_coroutine_deopt_resumes(self):
        class PauseOnce:
            def __await__(self):
                received = yield "pause"
                return "resumed" if received is None else received

        async def coro():
            return await PauseOnce()

        self.assertTrue(cinderx.jit.force_compile(coro))
        suspended = coro()
        self.assertEqual(suspended.send(None), "pause")
        self.assertTrue(_deopt_gen(suspended))

        with self.assertRaises(StopIteration) as caught:
            suspended.send(None)
        self.assertEqual(caught.exception.value, "resumed")


if __name__ == "__main__":
    unittest.main()
