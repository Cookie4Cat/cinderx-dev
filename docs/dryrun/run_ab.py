#!/usr/bin/env python3
"""M9 dry-run A/B: stock CPython 3.11 vs 3.11 + cinderx JIT.

Reference protocol from the 3.14 runs: auto-JIT threshold 2, pyperf
warmups 3. Dry-run rigor: --processes 3 --values 5 (formal runs use
pyperformance defaults, ~20 processes).

B-side scoping (dry-run caveat): CI_JIT_AUTO_ONLY_PREFIX confines
compilation to the workload files listed per benchmark, because organic
deopt-resume on the full stdlib surface has an open crash case (see
M9-log). Benchmarks whose hot code lives outside the prefix therefore
understate JIT effect.

Usage (inside container):
  python3.11 run_ab.py --out /tmp/m9ab [--benches a,b,c]
"""

import argparse
import json
import os
import statistics
import subprocess
import sys

SITE = "/opt/python/cp311-cp311/lib/python3.11/site-packages"
BM_ROOT = os.path.join(SITE, "pyperformance", "data-files", "benchmarks")
STDLIB = "/opt/python/cp311-cp311/lib/python3.11"
CINDERX_PP = "/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib"
SC_DIR = "/tmp/m9sc"

# bench -> (bm dir, extra argv, extra compile prefixes for the B side)
BENCHES = {
    "go": ("bm_go", [], []),
    "hexiom": ("bm_hexiom", [], []),
    "richards": ("bm_richards", [], []),
    "richards_super": ("bm_richards_super", [], []),
    "deltablue": ("bm_deltablue", [], []),
    "raytrace": ("bm_raytrace", [], []),
    "generators": ("bm_generators", [], []),
    "unpickle_pure_python": (
        "bm_pickle",
        ["unpickle_pure_python"],
        [os.path.join(STDLIB, "pickle.py")],
    ),
    "pickle_pure_python": (
        "bm_pickle",
        ["pickle_pure_python"],
        [os.path.join(STDLIB, "pickle.py")],
    ),
    "chaos": ("bm_chaos", [], []),
    "nbody": ("bm_nbody", [], []),
    "regex_compile": (
        "bm_regex_compile",
        [],
        [os.path.join(STDLIB, "re")],
    ),
    "spectral_norm": ("bm_spectral_norm", [], []),
    "sqlglot_v2_parse": (
        "bm_sqlglot_v2",
        ["parse"],
        [os.path.join(SITE, "sqlglot")],
    ),
    "sqlglot_v2_transpile": (
        "bm_sqlglot_v2",
        ["transpile"],
        [os.path.join(SITE, "sqlglot")],
    ),
    "fannkuch": ("bm_fannkuch", [], []),
    "scimark": ("bm_scimark", [], []),
    "nqueens": ("bm_nqueens", [], []),
    "float": ("bm_float", [], []),
}

PERF_ARGS = ["--warmups", "3", "--values", "5", "--processes", "3"]


def ensure_sitecustomize():
    os.makedirs(SC_DIR, exist_ok=True)
    with open(os.path.join(SC_DIR, "sitecustomize.py"), "w") as f:
        f.write(
            "try:\n"
            "    import cinderx\n"
            "    cinderx.init()\n"
            "except Exception:\n"
            "    pass\n"
        )


def run_side(bench, side, out_json, timeout):
    bm_dir, extra_argv, extra_prefixes = BENCHES[bench]
    script = os.path.join(BM_ROOT, bm_dir, "run_benchmark.py")
    cmd = [sys.executable, script, *extra_argv, "-o", out_json, *PERF_ARGS]
    env = dict(os.environ)
    if side == "b":
        # 工作负载模块（richards 等）安装于 site-packages，编译前缀取
        # site-packages 全域 + 各基准的 stdlib 例外；stdlib 与冻结模块
        # 仍被排除（有机 deopt-resume 已立案，见 M9-log）。
        prefixes = ":".join([SITE, *extra_prefixes])
        env["PYTHONPATH"] = f"{SC_DIR}:{CINDERX_PP}"
        env["PYTHONJITAUTO"] = os.environ.get("M9_THRESHOLD", "2")
        env["CI_JIT_AUTO_ONLY_PREFIX"] = prefixes
        cmd += [
            "--inherit-environ",
            "PYTHONPATH,PYTHONJITAUTO,CI_JIT_AUTO_ONLY_PREFIX",
        ]
    if os.path.exists(out_json):
        os.unlink(out_json)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    if proc.returncode != 0 or not os.path.exists(out_json):
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-2:]
        return "FAIL rc=%s %s" % (proc.returncode, " | ".join(tail))
    return "OK"


def mean_of(out_json):
    with open(out_json) as f:
        data = json.load(f)
    values = []
    for bench in data["benchmarks"]:
        for run in bench.get("runs", []):
            values.extend(run.get("values", []))
    return statistics.mean(values) if values else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--benches", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ensure_sitecustomize()
    names = args.benches.split(",") if args.benches else list(BENCHES)

    results = {}
    for bench in names:
        row = {}
        for side in ("a", "b"):
            out_json = os.path.join(args.out, f"{bench}-{side}.json")
            status = run_side(bench, side, out_json, args.timeout)
            row[side] = status
            if status == "OK":
                row[f"{side}_mean"] = mean_of(out_json)
            print(f"[{bench}] {side}: {status}", flush=True)
        if row.get("a_mean") and row.get("b_mean"):
            row["ratio"] = row["a_mean"] / row["b_mean"]
            print(
                "[%s] ratio(stock/jit)=%.3fx (a=%.4fs b=%.4fs)"
                % (bench, row["ratio"], row["a_mean"], row["b_mean"]),
                flush=True,
            )
        results[bench] = row

    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(results, f, indent=1)

    ratios = [r["ratio"] for r in results.values() if "ratio" in r]
    if ratios:
        geo = statistics.geometric_mean(ratios)
        print(
            "GEOMEAN ratio(stock/jit) over %d benches: %.3fx"
            % (len(ratios), geo),
            flush=True,
        )


if __name__ == "__main__":
    main()
