# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Trigger-proof test drivers for the CPython 3.11 JIT gates.

Trigger-proof drivers defined by the development plan
(docs/cp311-jit-dev-plan.md, MR-01).  Each drives a workload in a child interpreter, collects the unified
trigger-proof report at child exit, fills in the harness-owned fields, and
judges the result against explicit expectations.  The judging contract is
what makes these problem-front-loading rather than smoke tests:

  * an expected trigger that did not fire is a failure, never a pass;
  * a worker that dies or produces no report is a failure, never a skip;
  * environment configuration is asserted inside the child, not assumed.

On the capability-gated build the default expectations assert the gate
(requests counted, nothing compiled, counters zero); execution-capable MRs
override the expectations per stage rather than editing the drivers.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ci_pipeline.jit311 import report as _report

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

# Runs inside every child before the payload: activate the evaluator and
# arrange for the trigger-proof report to be written at exit.
CHILD_PREAMBLE = """\
import atexit, json, os, sys
sys.path.insert(0, os.environ["JIT311_REPO_ROOT"])
import _cinderx
import cinderx
cinderx.init()
_cinderx.install_frame_evaluator()

from ci_pipeline.jit311 import report as _jit311_report

def _jit311_emit():
    snap = _jit311_report.snapshot()
    # Sampling point: Python atexit, which runs BEFORE _cinderx's
    # module_free (and therefore before jit::finalize()).  For the gated
    # stage this is the stronger claim -- zero live compiled functions
    # before finalize implies zero after it.  The post-finalize contract
    # itself is enforced at the C level (RuntimeTests TearDown asserts
    # State::kNotInitialized after Py_FinalizeEx); execution-stage MRs add
    # the C-level probe for nonzero populations.
    try:
        from cinderjit import get_compiled_functions
        snap["live_compiled_functions_at_exit"] = len(
            get_compiled_functions()
        )
    except ImportError:
        snap["live_compiled_functions_at_exit"] = 0
    with open(os.environ["JIT311_REPORT_PATH"], "w") as fp:
        json.dump(snap, fp)

atexit.register(_jit311_emit)
"""

Judge = Callable[[dict], list[str]]


@dataclass
class RunResult:
    name: str
    ok: bool
    errors: list[str]
    report: dict | None
    returncode: int

    def summary(self) -> str:
        state = "ok" if self.ok else "FAIL"
        lines = [f"runner {self.name}: {state} (rc={self.returncode})"]
        lines += [f"  - {err}" for err in self.errors]
        return "\n".join(lines)


@dataclass
class RunnerSpec:
    name: str
    payload: str
    env: dict[str, str] = field(default_factory=dict)
    judges: list[Judge] = field(default_factory=list)
    # Expected-rejection specs (an unshipped mode) produce no report.
    expect_report: bool = True
    # Environment variables the child must observe with exactly these
    # values; asserted inside the child so a lost variable turns red.
    asserted_env: dict[str, str] = field(default_factory=dict)
    expect_returncode: int = 0
    # For expected-rejection specs: the child's stderr must contain this
    # substring, so an unrelated failure with the same exit code cannot
    # impersonate the pinned rejection.
    expect_stderr_contains: str | None = None
    timeout: int = 300


def _env_assertions(asserted_env: dict[str, str]) -> str:
    lines = []
    for key, value in asserted_env.items():
        lines.append(
            f"assert os.environ.get({key!r}) == {value!r}, "
            f"('config not effective', {key!r}, os.environ.get({key!r}))"
        )
    return "\n".join(lines) + ("\n" if lines else "")


def run(spec: RunnerSpec, python: str | None = None) -> RunResult:
    """Run one workload child and judge its trigger-proof report."""
    python = python or sys.executable
    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="jit311-") as tmp:
        report_path = os.path.join(tmp, "report.json")
        # Inherited CinderX / PYTHONJIT configuration would silently change
        # what every driver measures; a child sees only what its spec sets.
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(spec.env)
        env["JIT311_REPORT_PATH"] = report_path
        harness_path = os.path.join(tmp, "harness.json")
        env["JIT311_HARNESS_PATH"] = harness_path
        env["JIT311_REPO_ROOT"] = str(REPO_ROOT)
        source = CHILD_PREAMBLE + _env_assertions(spec.asserted_env)
        source += spec.payload
        try:
            proc = subprocess.run(
                [python, "-c", source],
                env=env,
                capture_output=True,
                timeout=spec.timeout,
            )
            returncode = proc.returncode
        except subprocess.TimeoutExpired:
            return RunResult(spec.name, False, ["worker timeout"], None, -1)

        stderr_text = proc.stderr.decode(errors="replace")
        if returncode != spec.expect_returncode:
            errors.append(
                f"worker exited {returncode}, expected "
                f"{spec.expect_returncode}: {stderr_text[-400:]}"
            )
        if (
            spec.expect_stderr_contains is not None
            and spec.expect_stderr_contains not in stderr_text
        ):
            errors.append(
                "expected rejection marker "
                f"{spec.expect_stderr_contains!r} not found in stderr: "
                f"{stderr_text[-400:]}"
            )
        if not spec.expect_report:
            if os.path.exists(report_path):
                errors.append(
                    "expected-rejection spec produced a report; the payload "
                    "ran past the pinned rejection point"
                )
            return RunResult(spec.name, not errors, errors, None, returncode)
        if not os.path.exists(report_path):
            errors.append("worker produced no report (counts as a crash)")
            return RunResult(spec.name, False, errors, None, returncode)

        with open(report_path, encoding="utf8") as fp:
            snap = json.load(fp)
        if os.path.exists(harness_path):
            with open(harness_path, encoding="utf8") as fp:
                for key, value in json.load(fp).items():
                    if key in _report.HARNESS_FIELDS:
                        snap[key] = value
        snap["worker_crashes"] = 1 if returncode not in (0, spec.expect_returncode) else 0
        errors.extend(
            f"report schema: {err}" for err in _report.validate_schema(snap)
        )
        for judge in spec.judges:
            errors.extend(judge(snap))
        return RunResult(spec.name, not errors, errors, snap, returncode)


# --------------------------------------------------------------------------
# Judges

def expect(field_name: str, op: str, value: int) -> Judge:
    """Field comparison judge: op is one of ==, >."""

    def judge(snap: dict) -> list[str]:
        actual = snap.get(field_name)
        checks = {
            "==": actual == value,
            ">": isinstance(actual, int) and actual > value,
        }
        if not checks[op]:
            return [f"expected {field_name} {op} {value}, got {actual!r}"]
        return []

    return judge


def gate_holds() -> list[Judge]:
    """The capability-gate default: nothing compiled, nothing executed,
    no unknown refusals."""
    return [
        expect("evaluator_installed", "==", True),
        expect("executable_alloc_calls", "==", 0),
        expect("compiled_function_creations", "==", 0),
        expect("machine_code_entries", "==", 0),
        expect("machine_code_installed", "==", 0),
        expect("unknown_rejects", "==", 0),
        expect("live_compiled_functions_at_exit", "==", 0),
    ]


# --------------------------------------------------------------------------
# Workload payloads

HOT_DEF = """\
def hot(a, b):
    total = 0
    for i in range(a):
        total += i * b
    return total
"""

HOT_LOOP = HOT_DEF + """
for _ in range(ITERS):
    hot(50, 3)
"""

CHURN = """\
def make(i):
    def fn(x, _i=i):
        return x + _i
    return fn

for i in range(2000):
    fn = make(i)
    for _ in range(5):
        fn(i)
    del fn
"""


# --------------------------------------------------------------------------
# The drivers.  Stage-appropriate defaults; later MRs override judges.

def cold_compile_runner(*, judges: list[Judge] | None = None) -> RunnerSpec:
    """Compile requests raised immediately after function creation: the
    request happens BEFORE the first call, so the code object is genuinely
    cold (no quickening, no caches)."""
    payload = HOT_DEF + (
        "from cinderx import jit\n"
        "assert jit.force_compile(hot) is False, 'gate must refuse'\n"
        "hot(50, 3)\n"
    )
    return RunnerSpec(
        name="cold_compile",
        payload=payload,
        judges=judges if judges is not None else gate_holds(),
    )


def warm_compile_runner(*, judges: list[Judge] | None = None) -> RunnerSpec:
    """Compile requests raised after interpreter warm-up.  Warm is defined
    by evidence, not by an iteration count: the payload asserts the target
    sites actually specialized (the int += must reach BINARY_OP_ADD_INT and
    at least three instructions must have quickened forms) and reports the
    specialized-instruction count before it raises the request."""
    payload = "ITERS = 200\n" + HOT_LOOP + (
        "import dis\n"
        "plain = [i.opname for i in dis.get_instructions(hot)]\n"
        "adaptive = [i.opname for i in"
        " dis.get_instructions(hot, adaptive=True)]\n"
        "specialized = [b for a, b in zip(plain, adaptive) if a != b]\n"
        "assert 'BINARY_OP_ADD_INT' in specialized, specialized\n"
        "assert len(specialized) >= 3, specialized\n"
        "print('warm: %d instructions specialized' % len(specialized))\n"
        "from cinderx import jit\n"
        "assert jit.force_compile(hot) is False, 'gate must refuse'\n"
    )
    return RunnerSpec(
        name="warm_compile",
        payload=payload,
        judges=judges if judges is not None else gate_holds(),
    )


def auto_like_runner(
    *,
    threshold: int = 30,
    iters: int | None = None,
    judges: list[Judge] | None = None,
) -> RunnerSpec:
    """Scheduling requests raised organically by crossing the call
    threshold under observe mode."""
    if iters is None:
        iters = threshold * 4
    payload = f"ITERS = {iters}\n" + HOT_LOOP
    default = gate_holds() + [
        expect("compile_requests", ">", 0),
        expect("compile_rejected", ">", 0),
    ]
    return RunnerSpec(
        name="auto_like",
        payload=payload,
        env={
            "CINDERX_JIT_MODE": "observe",
            "PYTHONJITAUTO": str(threshold),
        },
        asserted_env={
            "CINDERX_JIT_MODE": "observe",
            "PYTHONJITAUTO": str(threshold),
        },
        judges=judges if judges is not None else default,
    )


def shadow_compile_runner(*, judges: list[Judge] | None = None) -> RunnerSpec:
    """Shadow mode: full compilation with no installation.  The mode ships
    with the front end; until then the evaluator rejects it explicitly at
    startup (never a silent fallback), and this driver pins that rejection.
    The front-end MR flips expect_returncode/expect_report and installs
    real judges."""
    payload = "ITERS = 50\n" + HOT_LOOP
    return RunnerSpec(
        name="shadow_compile",
        payload=payload,
        env={"CINDERX_JIT_MODE": "shadow"},
        asserted_env={"CINDERX_JIT_MODE": "shadow"},
        judges=judges if judges is not None else [],
        expect_returncode=1,
        expect_report=False,
        expect_stderr_contains=(
            "CINDERX_JIT_MODE=shadow is not accepted on CPython 3.11"
        ),
    )


def lifecycle_churn_runner(*, judges: list[Judge] | None = None) -> RunnerSpec:
    """Create, exercise and discard functions in bulk."""
    return RunnerSpec(
        name="lifecycle_churn",
        payload=CHURN,
        judges=judges if judges is not None else gate_holds(),
    )


def shutdown_repetition_runner(
    *, repetitions: int = 5, judges: list[Judge] | None = None
) -> list[RunnerSpec]:
    """The same workload run to interpreter shutdown N times; every
    repetition must exit cleanly and report identically."""
    specs = []
    for i in range(repetitions):
        specs.append(
            RunnerSpec(
                name=f"shutdown_repetition_{i + 1}_of_{repetitions}",
                payload="ITERS = 50\n" + HOT_LOOP,
                judges=judges if judges is not None else gate_holds(),
            )
        )
    return specs


def config_matrix_runner(
    combos: list[dict[str, str]] | None = None,
    *,
    judges: list[Judge] | None = None,
) -> list[RunnerSpec]:
    """One child per configuration combination; every variable is asserted
    inside the child so configuration loss turns red."""
    if combos is None:
        combos = [
            {"CINDERX_JIT_MODE": "observe", "PYTHONJITAUTO": "1"},
            {"CINDERX_JIT_MODE": "observe", "PYTHONJITAUTO": "4"},
            {"CINDERX_JIT_MODE": "observe", "PYTHONJITAUTO": "1000000"},
        ]
    specs = []
    for i, combo in enumerate(combos):
        default = gate_holds()
        threshold = int(combo.get("PYTHONJITAUTO", "0") or 0)
        if combo.get("CINDERX_JIT_MODE") == "observe" and 0 < threshold <= 100:
            default = default + [expect("compile_requests", ">", 0)]
        specs.append(
            RunnerSpec(
                name=f"config_matrix_{i}",
                payload="ITERS = 400\n" + HOT_LOOP,
                env=dict(combo),
                asserted_env=dict(combo),
                judges=judges if judges is not None else default,
            )
        )
    return specs


def corpus_completeness_runner(
    *, expected_modules: int = 9, judges: list[Judge] | None = None
) -> RunnerSpec:
    """Completion contract over the migrated corpus: every corpus module
    must import and enumerate, and the module count is asserted inside the
    child against the manifest -- a silently missing module exits nonzero
    and turns red.  (The unified report's target_modules_attempted field
    belongs to the frozen stdlib surface, published by the libtest gate;
    the corpus surface is asserted here, not reported there.)"""
    payload = f"""
from ci_pipeline.jit311 import corpus as _corpus
_modules = set()
_case_total = 0
for _module, _case_name, _fn in _corpus.iter_cases():
    _modules.add(_module)
    _case_total += 1
assert _case_total > 0
assert len(_modules) == {expected_modules}, (
    "corpus module surface shrank", sorted(_modules))
"""
    return RunnerSpec(
        name="corpus_completeness",
        payload=payload,
        judges=judges if judges is not None else gate_holds(),
    )

def pyperformance_completeness_runner(
    *, manifest: dict[str, tuple[str, ...]] | None = None
) -> RunnerSpec | None:
    """Completion-oriented pyperformance leg: every manifest task must
    finish and report exactly its manifest result names -- zero worker
    crashes.  Tasks and result names are different namespaces (a task may
    emit several results, none under its own name), so the judge compares
    result names against the manifest's result column, never against the
    task column.  Returns None when pyperformance is unavailable; the
    daily job treats that as a loud provisioning failure, never a skip.

    Scope note: this leg proves worker completion under the stock
    interpreter arm.  Evaluator activation inside pyperformance's own
    manager/worker processes requires the mode-propagation machinery (pth
    activation) that ships with the front-end MR; its attestation joins
    this leg there."""
    try:
        import pyperformance  # noqa: F401
    except ImportError:
        return None
    if manifest is None:
        manifest = load_pyperf_benchmarks()
    bench_arg = ",".join(manifest)
    expected = sorted({r for results in manifest.values() for r in results})
    payload = f"""
import json, os, subprocess, sys, tempfile
out = os.path.join(tempfile.mkdtemp(prefix="jit311-pyperf-"), "res.json")
proc = subprocess.run(
    [sys.executable, "-m", "pyperformance", "run",
     "--benchmarks={bench_arg}", "--fast", "--inherit-environ",
     "PYTHONPATH,JIT311_REPO_ROOT", "-o", out],
    capture_output=True, text=True, timeout=7200,
)
assert proc.returncode == 0, (
    "pyperformance failed rc=%d: %s" % (proc.returncode, proc.stderr[-400:])
)
with open(out) as fp:
    data = json.load(fp)
reported = {{
    n
    for n in (
        b.get("metadata", {{}}).get("name")
        for b in data.get("benchmarks", [])
    )
    if n
}}
from ci_pipeline.jit311.runners import pyperf_completion_errors
_errors = pyperf_completion_errors(set({expected!r}), reported)
assert not _errors, "; ".join(_errors)
"""
    return RunnerSpec(
        name="pyperformance_completeness",
        payload=payload,
        judges=gate_holds(),
        timeout=7800,
    )


LIBTEST_TARGET_MANIFEST = (
    REPO_ROOT / "ci_pipeline" / "jit311" / "data" / "libtest_target_modules.txt"
)


def load_libtest_target_manifest() -> list[str]:
    return [
        line.strip()
        for line in LIBTEST_TARGET_MANIFEST.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


PYPERF_BENCHMARKS = (
    REPO_ROOT / "ci_pipeline" / "jit311" / "data" / "pyperf_benchmarks.txt"
)


def load_pyperf_benchmarks() -> dict[str, tuple[str, ...]]:
    """The committed completion set, mapping each pyperformance TASK to the
    exact result names it must report (`task: result [result ...]` lines).

    The two columns live in different namespaces: --benchmarks selects
    tasks, but a task may emit several results and none under its own name
    (scimark reports scimark_sor, scimark_fft, ...).  Growing the set is a
    data edit, and the runner asserts the reported result-name set equals
    the manifest's expectation exactly."""
    mapping: dict[str, tuple[str, ...]] = {}
    seen_results: set[str] = set()
    for line in PYPERF_BENCHMARKS.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        task, colon, results_field = line.partition(":")
        task = task.strip()
        results = tuple(results_field.split())
        if not colon or not task or not results:
            raise SystemExit(
                f"pyperf benchmark manifest line is not "
                f"'task: result [result ...]': {line!r} "
                f"({PYPERF_BENCHMARKS})"
            )
        if task in mapping:
            raise SystemExit(
                f"pyperf benchmark manifest duplicates task {task!r}: "
                f"{PYPERF_BENCHMARKS}"
            )
        for result in results:
            # A result name must sit under its task; anything else means
            # swapped columns or a foreign row, not a new benchmark.
            if result != task and not result.startswith(task + "_"):
                raise SystemExit(
                    f"pyperf manifest result {result!r} does not belong "
                    f"to task {task!r}: {PYPERF_BENCHMARKS}"
                )
            if result in seen_results:
                raise SystemExit(
                    f"pyperf benchmark manifest duplicates result "
                    f"{result!r}: {PYPERF_BENCHMARKS}"
                )
            seen_results.add(result)
        mapping[task] = results
    if not mapping:
        raise SystemExit(
            f"pyperf benchmark manifest is empty: {PYPERF_BENCHMARKS}"
        )
    return mapping


def pyperf_completion_errors(
    expected: set[str], reported: set[str]
) -> list[str]:
    """Judge a pyperformance run's reported result names against the
    manifest expectation.  Exact equality in both directions: a missing
    name is an incomplete task, and an unexpected name is tool or manifest
    drift that must be an explicit same-change edit of the pin and the
    manifest."""
    errors = []
    missing = expected - reported
    if missing:
        errors.append(
            "expected results not reported (task did not complete): "
            f"{sorted(missing)!r}"
        )
    unexpected = reported - expected
    if unexpected:
        errors.append(
            "results outside the manifest (tool/manifest drift; update "
            f"the pin and the manifest together): {sorted(unexpected)!r}"
        )
    return errors

def run_all(python: str | None = None) -> list[RunResult]:
    """Run the stage-default suite: every driver with gate expectations."""
    specs: list[RunnerSpec] = [
        cold_compile_runner(),
        warm_compile_runner(),
        auto_like_runner(),
        shadow_compile_runner(),
        lifecycle_churn_runner(),
        corpus_completeness_runner(),
    ]
    specs += shutdown_repetition_runner()
    specs += config_matrix_runner()
    results = [run(spec, python=python) for spec in specs]
    return results


def unify(fields_path: str, out_path: str) -> int:
    """Assemble the unified run report as a real artifact: a fresh gate
    child supplies the runtime snapshot, the libtest leg's emission
    supplies the module surface, and strict validation refuses any field
    that is missing or null."""
    import json as _json

    spec = cold_compile_runner()
    result = run(spec)
    if not result.ok or result.report is None:
        for err in result.errors:
            print(f"unified-report: gate child failed: {err}")
        return 1
    unified = dict(result.report)
    # Leg-owned fields: the probe child is a trivial one-process run, so
    # its own values here (a tautological zero) must never stand in for
    # the surface legs' attestations.  Null them out; only the leg
    # emission may fill them, and strict validation refuses a gap.
    unified["target_modules_attempted"] = None
    unified["worker_crashes"] = None
    try:
        fields = _json.loads(Path(fields_path).read_text())
    except OSError as exc:
        print(f"unified-report: cannot read leg emission: {exc}")
        return 1
    foreign = set(fields) - {"target_modules_attempted", "worker_crashes"}
    if foreign:
        # live_compiled_functions_at_exit is the probe child's own sample;
        # a leg overriding it (or anything else) is a contract violation.
        print(f"unified-report: leg emitted non-leg fields: {foreign}")
        return 1
    unified.update(fields)
    errors = _report.validate_schema(unified, strict=True)
    # Typing alone is not the contract: the module surface must be exactly
    # the frozen manifest and the crash count must be zero -- a smaller
    # surface or a positive crash count is a red verdict, not a datum.
    frozen = len(load_libtest_target_manifest())
    if not errors:
        if unified["target_modules_attempted"] != frozen:
            errors.append(
                f"target_modules_attempted must equal the frozen manifest "
                f"({frozen}), got {unified['target_modules_attempted']}"
            )
        if unified["worker_crashes"] != 0:
            errors.append(
                f"worker_crashes must be zero, got "
                f"{unified['worker_crashes']}"
            )
    if errors:
        for err in errors:
            print(f"unified-report: {err}")
        return 1
    Path(out_path).write_text(
        _json.dumps(unified, indent=1, sort_keys=True) + "\n"
    )
    print(
        f"unified-report: all fields strictly typed, "
        f"target_modules_attempted="
        f"{unified['target_modules_attempted']}"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--unify" in argv:
        try:
            fields_path = argv[argv.index("--unify") + 1]
            out_path = argv[argv.index("-o") + 1]
        except (ValueError, IndexError):
            print("usage: runners --unify <fields.json> -o <out.json>")
            return 2
        return unify(fields_path, out_path)
    if "--pyperf" in argv:
        spec = pyperformance_completeness_runner()
        if spec is None:
            print(
                "jit311-runners: pyperformance is not provisioned in this "
                "environment; the completeness leg cannot run (provision "
                "the runner venv with the pinned pyperformance package)",
                file=sys.stderr,
            )
            return 1
        result = run(spec)
        print(result.summary())
        return 0 if result.ok else 1
    results = run_all()
    failed = [r for r in results if not r.ok]
    for result in results:
        print(result.summary())
    print(
        f"jit311-runners: {len(results) - len(failed)}/{len(results)} passed"
    )
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
