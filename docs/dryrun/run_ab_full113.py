#!/usr/bin/env python3
"""pyperformance 1.13 全量 A/B：stock 3.11 vs 3.11 + cinderx JIT（全表面）。

目的：在 SR2 清单定稿前全面摸底劣化/崩溃风险——覆盖 1.13 在 3.11 上
适用的全部基准（约 86 项，含依赖三方库的大类），而非预演的 19 项。

前置（容器内一次性）：
  pip install --target /tmp/pp113 pyperformance==1.13.0
  逐 bm 依赖装入 /tmp/ppdeps（独立目录，不污染基础环境；A/B 两侧同挂）

协议与 run_ab.py 一致：pyperf --processes 3 --warmups 3 --values 5；
B 侧 PYTHONJITAUTO=2 全表面编译（诚实口径）。ratio = stock/jit，>1 为
JIT 更快。summary.json 逐基准增量落盘，可中途查看。

用法：python3.11 run_ab_full113.py --out /tmp/full113 [--benches a,b]
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tomllib

PP113 = "/tmp/pp113"
BM_ROOT = os.path.join(PP113, "pyperformance", "data-files", "benchmarks")
DEPS = "/tmp/ppdeps"
CINDERX_PP = "/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib"
SC_DIR = "/tmp/m9sc"
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


def gate_ok(req):
    if not req or not req.startswith(">="):
        return True
    try:
        parts = req[2:].split(".")
        return sys.version_info >= (int(parts[0]), int(parts[1]))
    except (ValueError, IndexError):
        return True


def load_jobs():
    """MANIFEST 驱动：基准名 -> (bm 目录, extra_opts)，requires-python 过滤。"""
    jobs = {}
    in_benchmarks = False
    with open(os.path.join(BM_ROOT, "MANIFEST")) as f:
        for line in f:
            line = line.strip()
            if line.startswith("["):
                in_benchmarks = line == "[benchmarks]"
                continue
            if not in_benchmarks or not line or line.startswith("#"):
                continue
            if line.startswith("name\t"):
                continue
            name, meta = line.split("\t")
            if meta == "<local>":
                bm_dir = "bm_" + name
                metafile = os.path.join(BM_ROOT, bm_dir, "pyproject.toml")
            elif meta.startswith("<local:"):
                bm_dir = "bm_" + meta[len("<local:"):-1]
                metafile = os.path.join(BM_ROOT, bm_dir, f"bm_{name}.toml")
            else:
                continue
            with open(metafile, "rb") as mf:
                t = tomllib.load(mf)
            if not gate_ok(t.get("project", {}).get("requires-python", "")):
                continue
            extra = t.get("tool", {}).get("pyperformance", {}).get(
                "extra_opts", []
            )
            jobs[name] = (bm_dir, [str(x) for x in extra])
    return jobs


def run_side(job, side, out_json, timeout):
    bm_dir, extra_argv = job
    script = os.path.join(BM_ROOT, bm_dir, "run_benchmark.py")
    cmd = [sys.executable, script, *extra_argv, "-o", out_json, *PERF_ARGS]
    env = dict(os.environ)
    if side == "a":
        env["PYTHONPATH"] = DEPS
    else:
        env["PYTHONPATH"] = f"{SC_DIR}:{CINDERX_PP}:{DEPS}"
        env["PYTHONJITAUTO"] = os.environ.get("M9_THRESHOLD", "2")
        cmd += ["--inherit-environ", "PYTHONPATH,PYTHONJITAUTO"]
    if side == "a":
        cmd += ["--inherit-environ", "PYTHONPATH"]
    if os.path.exists(out_json):
        os.unlink(out_json)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout, env=env
        )
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    if proc.returncode != 0 or not os.path.exists(out_json):
        tail = (proc.stderr or proc.stdout).strip().splitlines()[-3:]
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
    ap.add_argument("--timeout", type=int, default=1200)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ensure_sitecustomize()
    jobs = load_jobs()
    names = args.benches.split(",") if args.benches else sorted(jobs)
    print(f"total benches: {len(names)}", flush=True)

    summary_path = os.path.join(args.out, "summary.json")
    results = {}
    for i, bench in enumerate(names, 1):
        if bench not in jobs:
            results[bench] = {"a": "UNKNOWN BENCH"}
            continue
        row = {}
        for side in ("a", "b"):
            out_json = os.path.join(args.out, f"{bench}-{side}.json")
            status = run_side(jobs[bench], side, out_json, args.timeout)
            row[side] = status
            if status == "OK":
                row[f"{side}_mean"] = mean_of(out_json)
            print(f"[{i}/{len(names)} {bench}] {side}: {status}", flush=True)
        if row.get("a_mean") and row.get("b_mean"):
            row["ratio"] = row["a_mean"] / row["b_mean"]
            print(
                "[%s] ratio(stock/jit)=%.3fx (a=%.4fs b=%.4fs)"
                % (bench, row["ratio"], row["a_mean"], row["b_mean"]),
                flush=True,
            )
        results[bench] = row
        with open(summary_path, "w") as f:
            json.dump(results, f, indent=1)

    ratios = {k: r["ratio"] for k, r in results.items() if "ratio" in r}
    if ratios:
        geo = statistics.geometric_mean(ratios.values())
        print("\n=== 劣化榜（ratio 升序，<1 为 JIT 更慢）===", flush=True)
        for k in sorted(ratios, key=ratios.get):
            print("%-40s %.3f" % (k, ratios[k]), flush=True)
        print(
            "GEOMEAN ratio(stock/jit) over %d benches: %.3fx"
            % (len(ratios), geo),
            flush=True,
        )
    bad = [k for k, r in results.items() if r.get("a") != "OK" or r.get("b") != "OK"]
    if bad:
        print("FAIL/TIMEOUT list: " + ", ".join(sorted(bad)), flush=True)


if __name__ == "__main__":
    main()
