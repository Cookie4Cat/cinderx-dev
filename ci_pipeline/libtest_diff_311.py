#!/usr/bin/env python3
"""CPython 3.11 Lib/test dual-mode differential gate.

Runs the stdlib test suite twice on the same interpreter -- arm A ("stock",
clean environment) and arm B (environment overrides, e.g. CinderX loaded with
JIT off) -- and compares per-module and per-case outcomes.  Only regressions
count: failures already present in arm A are absorbed as the environmental
baseline, so no skip list needs to be maintained.

The execution engine is regrtest itself (``python -m test -j N --junit-xml``),
which provides multiprocess scheduling and crash isolation natively on 3.11.

Subcommands:
  run   run one arm, write a normalized result JSON
  diff  compare two result JSONs, exit 1 on regressions
  gate  run arm A, run arm B, diff (the PR-gate entry point)

Examples:
  # Harness self-certification: stock vs stock must be empty.
  python3.11 libtest_diff_311.py gate --out /tmp/selfcert

  # CinderX JIT-off arm (env values owned by the runtime MRs).
  python3.11 libtest_diff_311.py gate --out /tmp/jitoff \\
      --env CINDERX_PLUGIN_ENABLE=1 --env CINDERX_EVAL_MODE=cinder \\
      --env CINDERX_JIT_DISABLE=1 --pythonpath-prepend /path/to/startup

  # Quick subset while iterating.
  python3.11 libtest_diff_311.py gate --out /tmp/q --tests test_int test_dict
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

CASE_STATES = ("pass", "failure", "error", "skipped")


def list_tests(python: str) -> list[str]:
    out = subprocess.run(
        [python, "-m", "test", "--list-tests"],
        check=True, capture_output=True, text=True,
    ).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


# regrtest's per-module result line, e.g.
#   0:00:01 load avg: 7.78 [131/440/19] test_extcall passed
#   0:00:00 load avg: 7.78 [  6/440/1] test.test_asyncio.test_base_events failed (uncaught exception)
RESULT_LINE = re.compile(
    r"\[ *\d+/\d+(?:/\d+)?\] (\S+)(?: process)? "
    r"(passed|failed|skipped|crashed)\b"
)


def parse_regrtest_modules(log_text: str, requested: list[str]) -> dict[str, str]:
    """Module verdicts from regrtest's own per-module result lines.

    junit is case-granular and silently loses doctest-only modules and
    modules whose test classes live in alias packages (datetimetester,
    ctypes.test, unittest.test, ...), so the module verdict comes from the
    runner itself: passed / failed (any qualifier, env changed included) /
    crashed / skipped (resource denied included).  A requested module with
    no result line is no_result -- its worker died before reporting -- and
    the diff treats that as a regression whenever it is not symmetric.
    """
    wanted = set(requested)
    verdicts: dict[str, str] = {}
    for match in RESULT_LINE.finditer(log_text):
        name, word = match.group(1), match.group(2)
        if name not in wanted:
            continue
        if word == "passed":
            verdicts[name] = "pass"
        elif word == "skipped":
            verdicts[name] = "skip"
        elif word == "crashed":
            # A crashed worker is never comparable evidence: the verdict
            # stays distinct so it can neither launder into "fail" (where a
            # symmetric crash would diff as baseline) nor count as a result.
            verdicts[name] = "crash"
        else:
            verdicts[name] = "fail"
    return verdicts


def crash_count(modules: dict[str, str]) -> int:
    return sum(1 for verdict in modules.values() if verdict == "crash")


# regrtest exit codes that still denote a COMPLETED run: 0 (all passed),
# 2 (some tests failed), 3 (environment changed).  Anything else --
# signals (negative), interrupt (130), internal error, no-tests-ran --
# means the main process did not finish normally, and per-module verdict
# lines cannot certify such a run.
ACCEPTED_REGRTEST_EXITS = (0, 2, 3)

# Harness-internal failures that regrtest reports WITHOUT killing the main
# process: a worker thread that dies after its module already printed a
# verdict still ends in a normal FAILURE epilogue with exit code 2, and
# per-module verdicts alone would certify the run (3.11.6
# libregrtest/runtest_mp.py).
FATAL_HARNESS_MARKERS = ("regrtest worker thread failed",)


def arm_run_completed(returncode: int, log_text: str) -> str | None:
    if returncode not in ACCEPTED_REGRTEST_EXITS:
        return (
            f"regrtest ended abnormally (exit {returncode}); per-module "
            f"verdicts cannot certify a run whose main process died"
        )
    if "== Tests result: " not in log_text:
        return (
            "regrtest log has no completion epilogue; the run did not "
            "finish normally"
        )
    for marker in FATAL_HARNESS_MARKERS:
        if marker in log_text:
            return (
                f"regrtest reported a harness-internal failure "
                f"({marker!r}); the run cannot be certified"
            )
    return None


# Startup file the gate provisions for the CinderX arm.  Every interpreter
# in the arm installs the evaluator and appends an attestation line, so
# "the evaluator was live during the differential" is recorded evidence
# rather than an assumption.
STARTUP_SITECUSTOMIZE = '''\
"""CinderX diffgate arm startup: install the evaluator, attest, fail loud."""
import os

try:
    import _cinderx
    import cinderx
except ModuleNotFoundError:
    # Interpreters spawned by tests (fresh venvs) have no cinderx and must
    # behave exactly like stock.
    pass
else:
    try:
        cinderx.init()
        _cinderx.install_frame_evaluator()
        installed = bool(_cinderx.is_frame_evaluator_installed())
    except Exception:
        # site.py would print this and keep going, silently degrading the
        # arm to stock and turning the differential into a false neutral.
        # Dying here makes the module no_result on one arm only -- red.
        import traceback

        traceback.print_exc()
        os._exit(78)
    if not installed:
        import sys

        print("cinderx evaluator not installed after install call",
              file=sys.stderr)
        os._exit(78)
    attest = os.environ.get("CINDERX_DIFF_ATTEST")
    if attest:
        try:
            with open(attest, "a", encoding="utf-8") as fh:
                fh.write(f"{os.getpid()} {installed}\\n")
        except OSError:
            # A test child running under a dropped-privilege user may not be
            # able to sign the ledger.  The evaluator *is* installed -- the
            # only thing worth dying for -- and the parent processes carry
            # the attestation proof.
            pass
'''


def read_attest(path: Path) -> tuple[int, bool]:
    """Number of attested processes and whether every one was installed."""
    if not path.is_file():
        return 0, True
    rows = [line.split() for line in path.read_text().splitlines() if line.strip()]
    return len(rows), all(len(row) == 2 and row[1] == "True" for row in rows)


def missing_verdicts(modules: dict[str, str]) -> int:
    """Requested modules that produced no verdict at all.

    A healthy arm reports a verdict for every requested module -- pass,
    fail or skip.  Anything without one means a worker died or regrtest
    crashed mid-flight, and the arm must fail loudly: if both arms crashed
    the same way, the per-module diff would compare no_result against
    no_result and wave a broken run through as a false green.  regrtest's
    exit code cannot serve here, because baseline failures already make it
    non-zero on a perfectly healthy run.
    """
    return sum(1 for verdict in modules.values() if verdict == "no_result")


def case_state(tc: ET.Element) -> str:
    for child in tc:
        tag = child.tag.rsplit("}", 1)[-1]
        if tag in ("failure", "error", "skipped"):
            return tag
    return "pass"


def _normalize(name: str) -> str:
    return name[5:] if name.startswith("test.") else name


def make_module_resolver(requested: list[str]):
    """Map a junit classname to the requested test name it belongs to.

    ``--list-tests`` mixes top-level names (test_grammar) with package paths
    (test.test_asyncio.test_events); junit classnames are full dotted paths.
    Longest-prefix match on dot boundaries after stripping the "test." prefix
    keys every case back to its requested name.
    """
    normalized = sorted((_normalize(r), r) for r in requested)
    by_length = sorted(normalized, key=lambda nr: len(nr[0]), reverse=True)
    cache: dict[str, str] = {}

    def resolve(classname: str) -> str:
        cls = _normalize(classname)
        if cls in cache:
            return cache[cls]
        for norm, req in by_length:
            if cls == norm or cls.startswith(norm + "."):
                cache[cls] = req
                return req
        for part in cls.split("."):
            if part.startswith("test_"):
                cache[cls] = part
                return part
        cache[cls] = cls or "unknown"
        return cache[cls]

    return resolve


def parse_junit(path: Path) -> dict[str, str]:
    cases: dict[str, str] = {}
    root = ET.parse(path).getroot()
    for ts in root.iter():
        if ts.tag.rsplit("}", 1)[-1] != "testcase":
            continue
        classname = ts.get("classname") or ""
        name = ts.get("name") or ""
        # regrtest's junit output puts the full dotted path in `name` and
        # leaves `classname` empty; joining blindly would poison keys with a
        # leading dot.
        key = f"{classname}.{name}" if classname else name
        cases[key] = case_state(ts)
    return cases


SANITIZED_ENV_KEYS = (
    "PYTHONPATH", "PYTHONSTARTUP", "PYTHONHOME", "PYTHONHASHSEED",
)
SANITIZED_ENV_PREFIXES = ("CINDERX_", "PYTHONJIT", "PARALLEL_GC_")


def arm_environment(base: dict) -> dict:
    """Arms start from a sanitized environment: an inherited PYTHONPATH,
    sitecustomize hook, or stray CINDERX_* variable could activate the
    evaluator (or anything else) in BOTH arms and manufacture a false
    neutral.  Each arm re-adds exactly what it declares."""
    env = {
        key: value
        for key, value in base.items()
        if key not in SANITIZED_ENV_KEYS
        and not key.startswith(SANITIZED_ENV_PREFIXES)
    }
    # Unconditional: an inherited PYTHONHASHSEED=random would make the two
    # arms diverge for reasons that have nothing to do with the evaluator.
    env["PYTHONHASHSEED"] = "0"
    return env


def run_arm(args: argparse.Namespace) -> int:
    python = args.python
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    junit = out / "junit.xml"

    env = arm_environment(dict(os.environ))
    for kv in args.env or []:
        key, _, value = kv.partition("=")
        env[key] = value
    if args.pythonpath_prepend:
        prev = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = os.pathsep.join(args.pythonpath_prepend + ([prev] if prev else []))

    attest_path = (
        Path(args.attest_file) if getattr(args, "attest_file", None) else None
    )
    if attest_path is not None:
        attest_path.parent.mkdir(parents=True, exist_ok=True)
        attest_path.unlink(missing_ok=True)
        # World-writable so even test children that drop privileges can
        # sign the ledger.
        attest_path.touch()
        os.chmod(attest_path, 0o666)
        env["CINDERX_DIFF_ATTEST"] = str(attest_path)

    tests = args.tests or load_target_manifest()
    excluded = set(args.exclude or [])
    tests = [t for t in tests if t not in excluded]
    cmd = [
        python, "-u", "-m", "test",
        "-j", str(args.jobs),
        "--timeout", str(args.timeout),
        "--junit-xml", str(junit),
        *tests,
    ]
    started = time.time()
    # Stream regrtest's output straight to disk so a running arm can be
    # watched with tail -f -- which is exactly what diagnosing a hanging
    # test module needs.  The child runs with -u, so lines land live.
    with (out / "regrtest.log").open("w", encoding="utf-8") as sink:
        proc = subprocess.run(
            cmd, env=env, text=True, stdout=sink, stderr=subprocess.STDOUT
        )

    cases = parse_junit(junit) if junit.is_file() else {}
    log_text = (out / "regrtest.log").read_text(errors="replace")
    verdicts = parse_regrtest_modules(log_text, tests)
    modules: dict[str, str] = {name: verdicts.get(name, "no_result") for name in tests}

    attest_count, attest_ok = (
        read_attest(attest_path) if attest_path is not None else (0, True)
    )

    result = {
        "meta": {
            "python": python,
            "argv_tests": len(tests),
            "jobs": args.jobs,
            "env_overrides": args.env or [],
            "duration_s": round(time.time() - started, 1),
            "regrtest_exit": proc.returncode,
            "pythonpath_prepend": list(args.pythonpath_prepend or []),
            "attest_processes": attest_count,
            "attest_all_installed": attest_ok,
        },
        "modules": modules,
        "cases": cases,
    }
    (out / "result.json").write_text(json.dumps(result, indent=1, sort_keys=True))
    completion_error = arm_run_completed(proc.returncode, log_text)
    if completion_error:
        print(f"arm FAILED: {completion_error}")
        return 7
    counts = {
        s: sum(1 for v in modules.values() if v == s)
        for s in ("pass", "fail", "skip", "crash", "no_result")
    }
    print(f"arm done: {counts} duration={result['meta']['duration_s']}s -> {out / 'result.json'}")
    crashes = crash_count(modules)
    if crashes:
        print(
            f"arm FAILED: {crashes} module worker(s) crashed; a crash is "
            f"never comparable evidence, symmetric or not"
        )
        return 6
    gaps = missing_verdicts(modules)
    if gaps:
        print(
            f"arm FAILED: {gaps} modules produced no verdict (worker death "
            f"or a harness crash); regrtest exit {proc.returncode}"
        )
        return 5
    if attest_path is not None:
        print(f"arm attest: {attest_count} processes, all installed: {attest_ok}")
        # Fewer than two records means not even the regrtest main process
        # plus one worker attested: the arm cannot have been live.
        if attest_count < 2 or not attest_ok:
            print("arm attest FAILED: the evaluator was not verifiably live")
            return 4
    return 0


def load(path: str) -> dict:
    return json.loads(Path(path).read_text())


TARGET_MANIFEST = (
    Path(__file__).resolve().parent
    / "jit311" / "data" / "libtest_target_modules.txt"
)


def load_target_manifest() -> list[str]:
    """The frozen stdlib target surface.  The gate runs exactly this list:
    a dynamically discovered surface would shrink symmetrically with the
    environment and no one would notice."""
    if not TARGET_MANIFEST.is_file():
        raise SystemExit(f"target-module manifest missing: {TARGET_MANIFEST}")
    modules = [
        line.strip()
        for line in TARGET_MANIFEST.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if len(modules) != len(set(modules)):
        raise SystemExit("target-module manifest contains duplicates")
    return modules


def diff_results(a: dict, b: dict) -> dict:
    regressions: dict[str, dict[str, str]] = {}
    warnings: dict[str, dict[str, str]] = {}

    for mod, averdict in a["modules"].items():
        bverdict = b["modules"].get(mod, "missing")
        if averdict == bverdict:
            continue
        entry = {"stock": averdict, "cinderx": bverdict}
        # Regression: anything that was passing and no longer is -- a module
        # that starts *skipping* under the evaluator changed behavior too.
        if averdict == "pass" and bverdict in (
            "fail", "skip", "crash", "no_result", "missing"
        ):
            regressions[mod] = entry
        else:
            warnings[mod] = entry

    case_regressions: dict[str, dict[str, str]] = {}
    bcases = b.get("cases", {})
    bmodules = b.get("modules", {})
    resolve = make_module_resolver(list(a.get("modules", {})))
    for key, astate in a.get("cases", {}).items():
        if astate != "pass":
            continue
        bstate = bcases.get(key, "missing")
        if bstate in ("failure", "error", "skipped"):
            # A case that starts skipping under the evaluator shrank
            # coverage while staying green -- that is a regression here,
            # same as the module-level pass->skip rule above.
            case_regressions[key] = {"stock": astate, "cinderx": bstate}
        elif bstate == "missing":
            # A case that vanished from a module which still reported is a
            # regression in its own right.  When the whole module produced no
            # result, the module-level entry already carries that signal and
            # repeating it for every case would bury it.
            if bmodules.get(resolve(key)) not in ("no_result", "missing", "skip"):
                case_regressions[key] = {"stock": astate, "cinderx": bstate}

    return {
        "module_regressions": regressions,
        "module_warnings": warnings,
        "case_regressions": case_regressions,
    }


def cmd_diff(args: argparse.Namespace) -> int:
    report = diff_results(load(args.a), load(args.b))
    out = Path(args.report) if args.report else None
    text = json.dumps(report, indent=1, sort_keys=True)
    if out:
        out.write_text(text)
    print(text if len(text) < 8000 else text[:8000] + "\n... (truncated, see report file)")
    red = report["module_regressions"] or report["case_regressions"]
    print(f"DIFF: {len(report['module_regressions'])} module regressions, "
          f"{len(report['case_regressions'])} case regressions, "
          f"{len(report['module_warnings'])} warnings")
    return 1 if red else 0


STOCK_SITECUSTOMIZE = '''\
"""Stock control arm startup: prove cinderx never activated here."""
import atexit
import os
import sys


def _attest_stock_purity():
    path = os.environ.get("CINDERX_STOCK_ATTEST")
    if not path:
        return
    polluted = [
        name for name in ("cinderx", "_cinderx", "cinderjit")
        if name in sys.modules
    ]
    with open(path, "a", encoding="utf8") as fp:
        fp.write(("POLLUTED:" + ",".join(polluted)) if polluted else "clean")
        fp.write("\\n")


atexit.register(_attest_stock_purity)
'''


def read_stock_attest(path: Path) -> tuple[int, bool]:
    if not path.is_file():
        return 0, False
    lines = [ln for ln in path.read_text().splitlines() if ln]
    return len(lines), bool(lines) and all(ln == "clean" for ln in lines)


def cmd_gate(args: argparse.Namespace) -> int:
    out = Path(args.out)
    if not args.tests and args.exclude:
        # On the frozen surface the committed manifest is the only way to
        # shrink coverage; an --exclude here would mint a strictly valid
        # but smaller report.
        print("GATE: --exclude is not allowed on the frozen surface; "
              "edit the target-module manifest deliberately")
        return 2
    out.mkdir(parents=True, exist_ok=True)

    # The gate provisions its own startup for the CinderX arm: every
    # interpreter installs the evaluator, attests to it per process, and
    # dies loudly if installation fails.
    startup = out / "startup"
    startup.mkdir(parents=True, exist_ok=True)
    (startup / "sitecustomize.py").write_text(STARTUP_SITECUSTOMIZE)
    attest = out / "cinderx" / "attest.log"

    # The stock arm is a CONTROL: it gets its own startup whose only job
    # is to attest, per process, that no cinderx module ever loaded.  A
    # polluted control (external sitecustomize, inherited environment)
    # would otherwise fake neutrality symmetrically.
    stock_startup = out / "startup-stock"
    stock_startup.mkdir(parents=True, exist_ok=True)
    (stock_startup / "sitecustomize.py").write_text(STOCK_SITECUSTOMIZE)
    stock_attest = out / "stock" / "attest-stock.log"
    stock_attest.parent.mkdir(parents=True, exist_ok=True)
    stock_attest.unlink(missing_ok=True)
    stock_attest.touch()
    os.chmod(stock_attest, 0o666)

    a = argparse.Namespace(**{
        **vars(args),
        "out": str(out / "stock"),
        "env": [f"CINDERX_STOCK_ATTEST={stock_attest}"],
        "pythonpath_prepend": [str(stock_startup)],
        "attest_file": None,
    })
    b = argparse.Namespace(**{
        **vars(args),
        "out": str(out / "cinderx"),
        "pythonpath_prepend": [str(startup)] + list(args.pythonpath_prepend or []),
        "attest_file": str(attest),
    })
    if run_arm(a) or run_arm(b):
        return 2

    # Structural stock-arm purity: it has no evaluator startup, so an
    # evaluator attestation there means the arms were misrouted.
    stray = out / "stock" / "attest.log"
    if stray.exists():
        print(f"GATE: stock arm unexpectedly attested an evaluator: {stray}")
        return 3
    # Positive stock purity: every stock process must have attested that
    # no cinderx module ever loaded.  Fewer than two records means not
    # even the regrtest main process plus one worker reported.
    stock_count, stock_clean = read_stock_attest(stock_attest)
    if stock_count < 2 or not stock_clean:
        print(
            f"GATE: stock arm purity not attested "
            f"({stock_count} records, clean={stock_clean}): {stock_attest}"
        )
        return 3

    args2 = argparse.Namespace(a=str(out / "stock" / "result.json"),
                               b=str(out / "cinderx" / "result.json"),
                               report=str(out / "diff.json"))
    rc = cmd_diff(args2)

    # Trigger-proof bookkeeping: this leg IS the target-module surface of
    # the unified report (dev plan MR-01).  On the frozen surface it must
    # publish the attempted count, the count must equal the committed
    # manifest, and a publication failure is red -- a silently absent
    # field would let the aggregator downstream go hungry unnoticed.
    # Explicit --tests slices are a development tool and do not publish.
    if not args.tests:
        try:
            cinderx_result = json.loads(
                (out / "cinderx" / "result.json").read_text()
            )
            attempted = len(cinderx_result["modules"])
            frozen = len(load_target_manifest())
            if attempted != frozen:
                print(
                    f"GATE: attempted {attempted} modules but the frozen "
                    f"surface is {frozen}; the target surface shrank"
                )
                return 4
            (out / "trigger_report_fields.json").write_text(
                json.dumps(
                    {
                        "target_modules_attempted": attempted,
                        # Both arms returned: any crashed worker already
                        # failed the arm (verdict "crash", return 6), so
                        # this is an attested zero over the frozen
                        # surface, not a default.
                        "worker_crashes": 0,
                    }
                )
                + "\n"
            )
        except SystemExit:
            raise
        except Exception as exc:  # noqa: BLE001
            print(f"GATE: could not publish target_modules_attempted: {exc}")
            return 4
    return rc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    def common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--python", default=sys.executable)
        p.add_argument("--jobs", type=int, default=min(48, os.cpu_count() or 8))
        p.add_argument("--timeout", type=int, default=1200)
        p.add_argument("--env", action="append",
                       help="KEY=VALUE override for the (cinderx) arm; repeatable")
        p.add_argument("--pythonpath-prepend", action="append", default=[])
        p.add_argument("--tests", nargs="*", help="subset; default = full --list-tests")
        p.add_argument("--exclude", action="append", default=[],
                       help="module to drop from the corpus; repeatable")

    p_run = sub.add_parser("run", help="run one arm")
    common(p_run)
    p_run.add_argument("--out", required=True)
    p_run.add_argument("--attest-file",
                       help="record per-process evaluator attestations here "
                            "and fail the arm unless they all check out")
    p_run.set_defaults(func=run_arm)

    p_diff = sub.add_parser("diff", help="compare two result JSONs")
    p_diff.add_argument("a", help="stock result.json")
    p_diff.add_argument("b", help="cinderx result.json")
    p_diff.add_argument("--report")
    p_diff.set_defaults(func=cmd_diff)

    p_gate = sub.add_parser("gate", help="run both arms and diff")
    common(p_gate)
    p_gate.add_argument("--out", required=True)
    p_gate.set_defaults(func=cmd_gate)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
