#!/usr/bin/env python3
"""Basic Lib/test differential gate (M0 corpus layer 4, curated subset).

Runs each CPython stdlib test module twice with the *current* interpreter:
  interp - cinderx never loaded (the oracle)
  jit    - cinderx injected via a generated sitecustomize + JIT env

and diffs the per-module outcome (PASS / FAIL / CRASH:<sig> / TIMEOUT).
Environment-specific failures are absorbed by the oracle: only *divergence*
between the two modes counts. Baseline file shares the diffgate format.

Runs directly in the target environment (bare metal or container); the only
requirements are a python3.11 with the stdlib test package and cinderx
importable in the jit mode (PYTHONPATH set by the caller).

Usage:
  python3.11 run_libtest_diff.py --out report.json \
      [--modules-file libtest_modules_basic.txt] \
      [--jit-env PYTHONJITALL=1] [--baseline BASE] [--update-baseline BASE]

Exit codes: 0 pass (diffs subset of baseline), 1 new divergence, 2 infra error.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODULES = os.path.join(HERE, "libtest_modules_basic.txt")

SITECUSTOMIZE = """\
import os
if os.environ.get("DIFFGATE_JIT") == "1":
    import cinderx
    cinderx.init()
    # M2 config-2: route all frames through the vendored 3.11 loop.
    cinderx.install_frame_evaluator()
"""


def load_modules(path):
    mods = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                mods.append(line)
    return mods


def run_one(module, jit, jit_env, timeout, sc_dir):
    env = dict(os.environ)
    env["PYTHONHASHSEED"] = "0"
    if jit:
        env["DIFFGATE_JIT"] = "1"
        env["PYTHONPATH"] = sc_dir + os.pathsep + env.get("PYTHONPATH", "")
        for pair in jit_env:
            key, _, val = pair.partition("=")
            env[key] = val
    else:
        env.pop("DIFFGATE_JIT", None)
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "test", module],
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    if proc.returncode == 0:
        return "PASS", ""
    if proc.returncode < 0:
        return "CRASH:{}".format(-proc.returncode), proc.stderr[-400:]
    # regrtest exit 1 = test failures; capture the summary line if present
    summary = ""
    for line in proc.stdout.splitlines():
        if "Tests result:" in line or line.startswith("FAILED"):
            summary = line.strip()
    return "FAIL", summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules-file", default=DEFAULT_MODULES)
    ap.add_argument("--out", required=True)
    ap.add_argument("--baseline")
    ap.add_argument("--update-baseline")
    ap.add_argument("--jit-env", action="append", default=None,
                    help="KEY=VAL applied in jit mode (repeatable); default "
                         "PYTHONJITAUTO=24 (hotness threshold, matches the "
                         "3.14 libtest practice; use PYTHONJITALL=1 for the "
                         "aggressive compile-everything variant)")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()

    jit_env = args.jit_env if args.jit_env is not None else ["PYTHONJITAUTO=24"]
    modules = load_modules(args.modules_file)
    if not modules:
        print("libtest-diff: empty module list", file=sys.stderr)
        return 2

    failures = []
    detail = {}
    with tempfile.TemporaryDirectory() as sc_dir:
        with open(os.path.join(sc_dir, "sitecustomize.py"), "w") as f:
            f.write(SITECUSTOMIZE)
        for module in modules:
            interp_res, interp_note = run_one(
                module, False, jit_env, args.timeout, sc_dir)
            jit_res, jit_note = run_one(
                module, True, jit_env, args.timeout, sc_dir)
            status = "ok" if interp_res == jit_res else "DIVERGE"
            print("{:<28} interp={:<10} jit={:<12} {}".format(
                module, interp_res, jit_res, status))
            sys.stdout.flush()
            if interp_res != jit_res:
                fid = "libtest:{}".format(module)
                failures.append(fid)
                detail[fid] = {
                    "interp": interp_res, "interp_note": interp_note,
                    "jit": jit_res, "jit_note": jit_note,
                }

    failures.sort()
    baseline = []
    if args.baseline and os.path.exists(args.baseline):
        with open(args.baseline, encoding="utf-8") as f:
            baseline = json.load(f)["failures"]
    new = sorted(set(failures) - set(baseline))
    fixed = sorted(set(baseline) - set(failures))

    report = {
        "python": sys.version,
        "jit_env": jit_env,
        "modules": modules,
        "failures": failures,
        "new_failures": new,
        "fixed_vs_baseline": fixed,
        "detail": detail,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    if args.update_baseline:
        with open(args.update_baseline, "w", encoding="utf-8") as f:
            json.dump({"failures": failures}, f, indent=2)

    print("libtest-diff: {} modules, {} divergences ({} new, {} fixed)".format(
        len(modules), len(failures), len(new), len(fixed)))
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
