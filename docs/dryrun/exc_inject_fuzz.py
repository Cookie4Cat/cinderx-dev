#!/usr/bin/env python3
"""M6 exception-injection fuzz driver (count-then-inject, dry-run).

For every corpus module (jit mode, force-compiled via diffgate _bootstrap):
  1. count pass: CI_EXC_INJECT=count -> total checkpoints K (deterministic
     given fixed corpus order + PYTHONHASHSEED=0)
  2. inject sweep: pick up to --points evenly spaced checkpoint ordinals in
     [1, K]; rerun with CI_EXC_INJECT=<n> each time.

Verdict per injection run:
  CRASH     rc < 0 (signal: SEGV/abort) -> failure
  SURVIVED  process exited normally; the synthetic RuntimeError either
            surfaced in a CASE line / module traceback or was swallowed by
            a case's own except handler

Exit code 1 if any CRASH. Output: JSON report + console summary.

Usage:
  exc_inject_fuzz.py --diffgate <dir> [--points 8] [--modules a,b] --out r.json
"""

import argparse
import json
import os
import subprocess
import sys

BOOTSTRAP = "_bootstrap.py"


def run_one(diffgate, module, inject_env, timeout):
    env = dict(os.environ)
    env["PYTHONHASHSEED"] = "0"
    if inject_env is not None:
        env["CI_EXC_INJECT"] = inject_env
    else:
        env.pop("CI_EXC_INJECT", None)
    proc = subprocess.run(
        [sys.executable, BOOTSTRAP, "jit", "corpus", module, "0"],
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
        cwd=diffgate,
    )
    total = None
    for line in proc.stderr.splitlines():
        if line.startswith("CI_EXC_INJECT_TOTAL="):
            total = int(line.split("=", 1)[1])
    return proc.returncode, total, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diffgate", default="/src/scratch/m4-diffgate")
    ap.add_argument("--points", type=int, default=8)
    ap.add_argument("--modules", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    corpus_dir = os.path.join(args.diffgate, "corpus")
    modules = (
        args.modules.split(",")
        if args.modules
        else sorted(
            n[:-3]
            for n in os.listdir(corpus_dir)
            if n.startswith("corpus_") and n.endswith(".py")
        )
    )

    report = {}
    crashes = 0
    for module in modules:
        rc, total, _out, err = run_one(
            args.diffgate, module, "count", args.timeout
        )
        if total is None:
            print(f"[{module}] count pass failed rc={rc}; skip", flush=True)
            report[module] = {"error": f"count pass rc={rc}"}
            continue
        if args.points == 0:  # full sweep over every checkpoint
            points = list(range(1, total + 1))
        else:
            k = min(args.points, total) if total > 0 else 0
            points = sorted(
                {max(1, round(i * total / (k + 1))) for i in range(1, k + 1)}
            )
        entry = {
            "checkpoints": total,
            "count_rc": rc,
            "points": points,
            "runs": [],
        }
        for n in points:
            rc_n, _t, out_n, err_n = run_one(
                args.diffgate, module, str(n), args.timeout
            )
            injected_visible = "ci-exc-inject" in out_n or "ci-exc-inject" in err_n
            verdict = "CRASH" if rc_n < 0 else "SURVIVED"
            if verdict == "CRASH":
                crashes += 1
            entry["runs"].append(
                {
                    "at": n,
                    "rc": rc_n,
                    "verdict": verdict,
                    "injected_exc_visible": injected_visible,
                }
            )
            if verdict == "CRASH":
                tail = (err_n or out_n).strip().splitlines()[-3:]
                entry["runs"][-1]["tail"] = tail
        n_crash = sum(1 for r in entry["runs"] if r["verdict"] == "CRASH")
        n_vis = sum(1 for r in entry["runs"] if r["injected_exc_visible"])
        print(
            f"[{module}] checkpoints={total} swept={len(points)} "
            f"crash={n_crash} exc-visible={n_vis}",
            flush=True,
        )
        report[module] = entry

    with open(args.out, "w") as f:
        json.dump(report, f, indent=1)
    total_runs = sum(len(e.get("runs", [])) for e in report.values())
    print(f"total injection runs={total_runs} crashes={crashes}")
    sys.exit(1 if crashes else 0)


if __name__ == "__main__":
    main()
