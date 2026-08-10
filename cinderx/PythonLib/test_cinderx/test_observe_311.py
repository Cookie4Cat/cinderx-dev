# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Eval/Observe runtime mode on CPython 3.11.

Hot counting at the frame entry, exactly one scheduling request per hot code
object, the typed refusal at the compile entry point, and the startup
controls that select the mode.  Every test runs its scenario in a child
interpreter because the mode is parsed from the environment at install time.
"""

import json
import os
import sys
import tempfile
import unittest

from test.support.script_helper import assert_python_ok

REFUSAL = "CINDERX311_JIT_EXEC_DISABLED"
THRESHOLD = 30

PREAMBLE = """\
import json
import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()
"""


def run_child(source, **env):
    env.setdefault("CINDERX_JIT_MODE", "observe")
    env.setdefault("PYTHONJITAUTO", str(THRESHOLD))
    source = source.replace("@T@", str(THRESHOLD))
    rc, out, err = assert_python_ok("-c", source, **env)
    return json.loads(out.decode().strip().splitlines()[-1])


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Observe311Tests(unittest.TestCase):
    def test_threshold_boundary_and_dedup(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def hot(a, b):
    return a * b + 1

for i in range(@T@ - 1):
    if hot(i, 1) != i + 1:
        raise SystemExit("wrong result before threshold")
before = _cinderx._get_observe_stats()
value = hot(6, 7)
at = _cinderx._get_observe_stats()
for i in range(5):
    hot(i, 2)
after = _cinderx._get_observe_stats()

def mine(stats):
    return [e for e in stats["events"] if e["qualname"] == "hot"]

print(json.dumps({
    "before": mine(before),
    "at": mine(at),
    "after": mine(after),
    "value": value,
    "enabled": after["enabled"],
    "threshold": after["threshold"],
}))
"""
        )
        self.assertTrue(payload["enabled"])
        self.assertEqual(payload["threshold"], THRESHOLD)
        self.assertEqual(payload["value"], 43)
        # Nothing fires before the threshold.
        self.assertEqual(payload["before"], [])
        # The threshold-crossing entry fires exactly one event, and further
        # calls never fire another.
        self.assertEqual(len(payload["at"]), 1)
        event = payload["at"][0]
        self.assertEqual(event["count"], THRESHOLD)
        self.assertEqual(event["result"], REFUSAL)
        self.assertEqual(payload["after"], payload["at"])

    def test_recursion_emits_a_single_event(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import sys
sys.setrecursionlimit(10000)

def rec(n):
    if n <= 0:
        return 0
    return rec(n - 1) + 1

value = rec(@T@ * 3)
stats = _cinderx._get_observe_stats()
events = [e for e in stats["events"] if e["qualname"] == "rec"]
print(json.dumps({"value": value, "events": events}))
"""
        )
        self.assertEqual(payload["value"], THRESHOLD * 3)
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["count"], THRESHOLD)
        self.assertEqual(payload["events"][0]["result"], REFUSAL)

    def test_no_machine_code_after_the_event(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import cinderx.jit

def hot():
    total = 0
    for i in range(10):
        total += i
    return total

for _ in range(@T@ + 10):
    hot()
stats = _cinderx._get_observe_stats()
print(json.dumps({
    "events": [e for e in stats["events"] if e["qualname"] == "hot"],
    "compiled": bool(cinderx.jit.is_jit_compiled(hot)),
    "value": hot(),
}))
"""
        )
        self.assertEqual(len(payload["events"]), 1)
        self.assertFalse(payload["compiled"])
        self.assertEqual(payload["value"], 45)

    def test_off_mode_observes_nothing(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
def hot(a):
    return a + 1

for i in range(@T@ * 2):
    hot(i)
print(json.dumps(_cinderx._get_observe_stats()))
""",
            CINDERX_JIT_MODE="off",
        )
        self.assertFalse(payload["enabled"])
        self.assertEqual(payload["events"], [])

    def test_execute_mode_refuses_the_takeover(self) -> None:
        payload = run_child(
            """\
import json
import _cinderx
import cinderx

cinderx.init()
try:
    _cinderx.install_frame_evaluator()
    raise SystemExit("install unexpectedly succeeded")
except RuntimeError as exc:
    message = str(exc)
print(json.dumps({
    "installed": _cinderx.is_frame_evaluator_installed(),
    "message": message,
}))
""",
            CINDERX_JIT_MODE="execute",
        )
        # The stock entry point stays in place and the reason is explicit.
        self.assertFalse(payload["installed"])
        self.assertIn("not accepted", payload["message"])

    def test_unusable_threshold_refuses_the_takeover(self) -> None:
        payload = run_child(
            """\
import json
import _cinderx
import cinderx

cinderx.init()
try:
    _cinderx.install_frame_evaluator()
    raise SystemExit("install unexpectedly succeeded")
except RuntimeError as exc:
    message = str(exc)
print(json.dumps({
    "installed": _cinderx.is_frame_evaluator_installed(),
    "message": message,
}))
""",
            PYTHONJITAUTO="auto",
        )
        self.assertFalse(payload["installed"])
        self.assertIn("positive integer", payload["message"])

    def test_observe_file_records_the_event(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "observe.log")
            run_child(
                PREAMBLE
                + """\
def hot(a):
    return a - 1

for i in range(@T@ + 3):
    hot(i)
print(json.dumps({"done": True}))
""",
                CINDERX_JIT_OBSERVE_FILE=path,
            )
            with open(path, encoding="utf-8") as recorded:
                lines = [line for line in recorded.read().splitlines() if "hot" in line]
        self.assertEqual(len(lines), 1)
        self.assertIn(str(THRESHOLD), lines[0])
        self.assertIn(REFUSAL, lines[0])

    def test_counters_follow_the_code_lifecycle(self) -> None:
        payload = run_child(
            PREAMBLE
            + """\
import gc

made = {}
for index in range(5):
    namespace = {}
    exec("def burst_" + str(index) + "(x):\\n    return x + " + str(index), namespace)
    fn = namespace["burst_" + str(index)]
    for _ in range(@T@):
        fn(1)
    made[index] = fn
del made, namespace, fn
gc.collect()

def late(x):
    return x * 2

for _ in range(@T@):
    late(3)
stats = _cinderx._get_observe_stats()
names = [e["qualname"] for e in stats["events"]]
print(json.dumps({"names": names}))
"""
        )
        names = payload["names"]
        expected = {f"burst_{i}" for i in range(5)} | {"late"}
        self.assertTrue(expected.issubset(set(names)))
        # One event per code object, including the ones already collected.
        for name in expected:
            self.assertEqual(names.count(name), 1)

    def test_startup_control_installs_on_request(self) -> None:
        probe = (
            "import _cinderx_auto, _cinderx, json; "
            "print(json.dumps(_cinderx.is_frame_evaluator_installed()))"
        )
        rc, out, err = assert_python_ok(
            "-c", probe, CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder"
        )
        self.assertTrue(json.loads(out.decode().strip().splitlines()[-1]))

        rc, out, err = assert_python_ok(
            "-c", probe, CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="stock"
        )
        self.assertFalse(json.loads(out.decode().strip().splitlines()[-1]))

    def test_startup_control_refuses_unknown_modes(self) -> None:
        from test.support.script_helper import assert_python_failure

        rc, out, err = assert_python_failure(
            "-c",
            "import _cinderx_auto",
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="jitter",
        )
        self.assertIn(b"not accepted", err)


if __name__ == "__main__":
    unittest.main()
