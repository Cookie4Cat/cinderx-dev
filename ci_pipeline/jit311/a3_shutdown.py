"""A3-F repeated real-process shutdown and finalize-state matrix."""

from __future__ import annotations

import argparse
import gc
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import types


STATES = (
    "installed",
    "parked",
    "function-death",
    "code-death",
    "failure-unwind",
    "multithread-completed",
)

FORBIDDEN_STDERR = (
    "Fatal Python error",
    "JIT_CHECK",
    "AddressSanitizer",
    "double free",
    "invalid pointer",
    "Exception ignored in",
)


def _plain(index: int):
    namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    source = (
        f"def shutdown_{index}(a, b, one):\n"
        "    total = a - a\n"
        "    i = total\n"
        "    while i < b:\n"
        "        total += a\n"
        "        i += one\n"
        f"    return total + {index}\n"
    )
    exec(compile(source, f"<a3-shutdown-{index}>", "exec"), namespace, namespace)
    return namespace[f"shutdown_{index}"], namespace, 15 + index


def child(state: str) -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    before = _cinderx._get_trigger_stats()["machine_code_entries"]
    roots = []

    owner, owner_namespace, expected = _plain(0)
    assert cinderjit.force_compile(owner) is True
    assert owner(3, 5, 1) == expected
    roots.extend((owner, owner_namespace))

    generator_namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
    exec(
        "def shutdown_generator(value):\n"
        "    yield value\n"
        "    yield value + 1\n",
        generator_namespace,
        generator_namespace,
    )
    generator_function = generator_namespace["shutdown_generator"]
    assert cinderjit.force_compile(generator_function) is True
    suspended_generator = generator_function(1)
    assert next(suspended_generator) == 1
    roots.extend((generator_namespace, generator_function, suspended_generator))

    for index in range(1, 9):
        fresh = types.FunctionType(owner.__code__, owner.__globals__, f"fresh_{index}")
        assert cinderjit.force_compile(fresh) is True
        assert fresh(3, 5, 1) == expected
        roots.append(fresh)

    if state == "parked":
        cinderjit.disable()
    elif state in ("function-death", "code-death"):
        for index in range(20, 40):
            transient, namespace, transient_expected = _plain(index)
            assert cinderjit.force_compile(transient) is True
            assert transient(3, 5, 1) == transient_expected
            del transient, namespace
        gc.collect()
        gc.collect()
    elif state == "failure-unwind":
        transient, namespace, transient_expected = _plain(50)
        assert cinderjit._jit311_compile_with_publish_failure(transient, 5) is True
        assert not cinderjit.is_jit_compiled(transient)
        assert cinderjit.force_compile(transient) is True
        assert transient(3, 5, 1) == transient_expected
        roots.extend((transient, namespace))
    elif state == "multithread-completed":
        assert cinderjit._jit311_multithreaded_compile_test_enabled() is True
        batch = []
        for index in range(60, 68):
            function, namespace, batch_expected = _plain(index)
            assert cinderjit._jit311_register_for_compile(function) is True
            batch.append((function, namespace, batch_expected))
        cinderjit._jit311_multithreaded_compile_test()
        for function, namespace, batch_expected in batch:
            assert cinderjit.is_jit_compiled(function)
            assert function(3, 5, 1) == batch_expected
        roots.extend(batch)

    # The object is deliberately rooted until module teardown.  Its finalizer
    # reaches only the read-only private control plane, so stderr exposes an
    # unsafe finalize ordering without changing the state being observed.
    class ExitFinalizer:
        def __init__(self, jit):
            self.jit = jit

        def __del__(self):
            state = self.jit._jit311_lifecycle_snapshot()
            assert state["schema"] == "cp311-jit-a3-lifecycle-v1"
            self.jit.is_enabled()

    roots.append(ExitFinalizer(cinderjit))
    snapshot = dict(cinderjit._jit311_lifecycle_snapshot())
    invariants = dict(cinderjit._jit311_lifecycle_invariants())
    entries = _cinderx._get_trigger_stats()["machine_code_entries"] - before
    assert entries > 0
    assert invariants.get("ok") is True, invariants
    # Keep the complete graph rooted to process shutdown.
    globals()["_A3_SHUTDOWN_ROOTS"] = roots
    return {
        "result": "READY_FOR_NORMAL_EXIT",
        "state": state,
        "machine_code_entries": entries,
        "snapshot": snapshot,
        "invariants": invariants,
    }


def driver(repetitions: int, child_timeout: int, out: Path) -> dict:
    rows = []
    failures = []
    for index in range(repetitions):
        state = STATES[index % len(STATES)]
        env = os.environ.copy()
        if state == "multithread-completed":
            env.update(
                PYTHONJITMULTITHREADEDCOMPILETEST="1",
                PYTHONJITBATCHCOMPILEWORKERS="4",
            )
        started = time.monotonic()
        timed_out = False
        try:
            process = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "ci_pipeline.jit311.a3_shutdown",
                    "--child",
                    "--state",
                    state,
                ],
                capture_output=True,
                text=True,
                env=env,
                timeout=child_timeout,
            )
            returncode = process.returncode
            stdout = process.stdout
            stderr = process.stderr
        except subprocess.TimeoutExpired as exc:
            timed_out = True
            returncode = 124
            stdout = exc.stdout or ""
            stderr = exc.stderr or ""
        forbidden = [token for token in FORBIDDEN_STDERR if token in stderr]
        payload = None
        try:
            payload = json.loads(stdout.strip().splitlines()[-1])
        except (IndexError, json.JSONDecodeError):
            pass
        errors = []
        if timed_out:
            errors.append("timeout")
        if returncode != 0:
            errors.append(f"exit code {returncode}")
        if forbidden:
            errors.append(f"forbidden stderr tokens: {forbidden}")
        if not payload or payload.get("result") != "READY_FOR_NORMAL_EXIT":
            errors.append("missing child readiness evidence")
        row = {
            "iteration": index + 1,
            "state": state,
            "returncode": returncode,
            "timed_out": timed_out,
            "duration_s": round(time.monotonic() - started, 3),
            "forbidden_stderr": forbidden,
            "child": payload,
            "stdout_tail": stdout[-1000:],
            "stderr_tail": stderr[-2000:],
            "errors": errors,
        }
        rows.append(row)
        if errors:
            failures.append(row)
    result = {
        "result": "PASS" if not failures else "FAIL",
        "repetitions": repetitions,
        "successful_exits": repetitions - len(failures),
        "states": list(STATES),
        "failures": failures,
        "rows": rows,
    }
    out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child", action="store_true")
    parser.add_argument("--state", choices=STATES, default="installed")
    parser.add_argument("--repetitions", type=int, default=100)
    parser.add_argument("--child-timeout", type=int, default=30)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args(argv)
    if args.child:
        print(json.dumps(child(args.state), sort_keys=True), flush=True)
        return 0
    if args.out is None:
        parser.error("--out is required in driver mode")
    result = driver(args.repetitions, args.child_timeout, args.out)
    print(json.dumps({"result": result["result"], "successful_exits": result["successful_exits"]}, sort_keys=True))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
