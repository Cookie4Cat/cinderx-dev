import json
import os
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 A1 infrastructure"
)
class A1Infra311Test(unittest.TestCase):
    def run_child(self, body: str):
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("CINDERX_", "PYTHONJIT", "PARALLEL_GC_"))
        }
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(body)],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return json.loads(proc.stdout.strip().splitlines()[-1])

    def test_compile_diagnostic_compiles_or_returns_typed_refusal(self):
        result = self.run_child(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def simple(value):
                return value + 1

            def subscr(value):
                return value[0]

            async def async_fn():
                return 1

            before = _cinderx._get_trigger_stats()
            payload = {
                "simple": cinderjit._jit311_compile_diagnostic(simple),
                "subscr": cinderjit._jit311_compile_diagnostic(subscr),
                "async": cinderjit._jit311_compile_diagnostic(async_fn),
            }
            after = _cinderx._get_trigger_stats()
            payload["creations_delta"] = (
                after["compiled_function_creations"]
                - before["compiled_function_creations"])
            payload["alloc_delta"] = (
                after["executable_alloc_calls"]
                - before["executable_alloc_calls"])
            print(json.dumps(payload, sort_keys=True))
            """
        )
        self.assertEqual(result["creations_delta"], 1)
        self.assertEqual(result["alloc_delta"], 1)
        self.assertEqual(
            result["simple"],
            {
                "eligible": True,
                "compiled": True,
                "phase": "compiler",
                "reason": None,
            },
        )
        self.assertEqual(result["subscr"]["reason"], "REFUSE_SHAPE_EXECUTE_SURFACE")
        self.assertFalse(result["subscr"]["eligible"])
        self.assertEqual(result["async"]["reason"], "REFUSE_SHAPE_ASYNC_CODE")
        self.assertFalse(result["async"]["eligible"])

    def test_compile_diagnostic_reports_runtime_fallback(self):
        result = self.run_child(
            """
            import json, sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def target(value):
                return value + 1

            def tracer(frame, event, arg):
                return tracer

            sys.settrace(tracer)
            traced = cinderjit._jit311_compile_diagnostic(target)
            sys.settrace(None)
            untraced = cinderjit._jit311_compile_diagnostic(target)
            print(json.dumps({"traced": traced, "untraced": untraced}, sort_keys=True))
            """
        )
        self.assertEqual(result["traced"]["phase"], "runtime")
        self.assertEqual(result["traced"]["reason"], "TRACING_ACTIVE")
        self.assertIsNone(result["untraced"]["reason"])

    def test_compile_diagnostic_rejects_non_function(self):
        result = self.run_child(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit
            try:
                cinderjit._jit311_compile_diagnostic(42)
            except TypeError as exc:
                payload = {"type": type(exc).__name__, "message": str(exc)}
            else:
                raise SystemExit("non-function diagnostic succeeded")
            print(json.dumps(payload, sort_keys=True))
            """
        )
        self.assertEqual(result["type"], "TypeError")
        self.assertIn("expected a Python function", result["message"])
