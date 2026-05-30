import os
import subprocess
import sys
import types
import unittest

import cinderx.jit


_FAST_ATTACH_SCRIPT = r"""
import cinderx.jit


def make_inner():
    def inner(x):
        return x + 1

    return inner


first = make_inner()
assert cinderx.jit.force_compile(first)
assert cinderx.jit.is_jit_compiled(first)

second = make_inner()
assert cinderx.jit.is_jit_compiled(second)
assert second(41) == 42
"""


_JIT_LIST_BAIL_SCRIPT = r"""
import cinderx.jit


def make_inner():
    def inner(x):
        return x + 1

    return inner


first = make_inner()
assert cinderx.jit.force_compile(first)
assert cinderx.jit.is_jit_compiled(first)

cinderx.jit.append_jit_list("not_the_module:not_the_function")

second = make_inner()
assert not cinderx.jit.is_jit_compiled(second)
assert second(41) == 42
"""


_INSTRUMENTATION_BAIL_SCRIPT = r"""
import sys

import cinderx.jit


def make_inner():
    def inner(x):
        return x + 1

    return inner


def profiler(*args):
    return None


first = make_inner()
assert cinderx.jit.force_compile(first)
assert cinderx.jit.is_jit_compiled(first)

sys.setprofile(profiler)
try:
    second = make_inner()
    assert not cinderx.jit.is_jit_compiled(second)
    assert second(41) == 42
finally:
    sys.setprofile(None)
"""


def _clean_jit_env() -> dict[str, str]:
    env = os.environ.copy()
    for name in (
        "CINDERX_DISABLE",
        "CINDERX_JIT_DISABLE",
        "PYTHONJITDISABLE",
        "PYTHONJITALL",
        "PYTHONJITALLSTATICFUNCTIONS",
        "PYTHONJITAUTO",
        "PYTHONJITLISTFILE",
    ):
        env.pop(name, None)
    env["CINDERX_PLUGIN_ENABLE"] = "1"
    return env


def _run_clean_jit_subprocess(script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-c", script],
        env=_clean_jit_env(),
        capture_output=True,
        text=True,
        timeout=60,
    )


@unittest.skipUnless(cinderx.jit.is_enabled(), "requires CinderX JIT")
class CachedCompiledEntryTests(unittest.TestCase):
    def assert_clean_jit_subprocess_succeeds(self, script: str) -> None:
        completed = _run_clean_jit_subprocess(script)

        self.assertEqual(
            completed.returncode,
            0,
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )

    def test_recreated_function_attaches_cached_compiled_entry(self) -> None:
        # Other test_cinderx cases can leave a process-local JIT list installed.
        # The cached-entry fast path intentionally stays disabled in that state.
        self.assert_clean_jit_subprocess_succeeds(_FAST_ATTACH_SCRIPT)

    def test_jit_list_disables_cached_compiled_entry_fast_path(self) -> None:
        self.assert_clean_jit_subprocess_succeeds(_JIT_LIST_BAIL_SCRIPT)

    def test_instrumentation_disables_cached_compiled_entry_fast_path(self) -> None:
        self.assert_clean_jit_subprocess_succeeds(_INSTRUMENTATION_BAIL_SCRIPT)

    def test_cached_entry_does_not_cross_globals(self) -> None:
        globs = {
            "value": "original",
            "__builtins__": __builtins__,
        }
        exec("def read_global():\n    return value\n", globs)
        read_global = globs["read_global"]

        self.assertTrue(cinderx.jit.force_compile(read_global))
        self.assertTrue(cinderx.jit.is_jit_compiled(read_global))
        self.assertEqual(read_global(), "original")

        other_globals = {
            "value": "other",
            "__builtins__": read_global.__globals__["__builtins__"],
        }
        other = types.FunctionType(
            read_global.__code__, other_globals, "read_global"
        )

        self.assertEqual(other(), "other")

    def test_cached_entry_does_not_cross_builtins(self) -> None:
        def make_function_with_builtin_marker(result):
            globs = {"__builtins__": {"marker": lambda: result}}
            exec("def call_marker():\n    return marker()\n", globs)
            return globs["call_marker"]

        first = make_function_with_builtin_marker("original")
        self.assertTrue(cinderx.jit.force_compile(first))
        self.assertTrue(cinderx.jit.is_jit_compiled(first))
        self.assertEqual(first(), "original")

        other = types.FunctionType(
            first.__code__,
            {"__builtins__": {"marker": lambda: "other"}},
            "call_marker",
        )

        self.assertEqual(other(), "other")


if __name__ == "__main__":
    unittest.main()
