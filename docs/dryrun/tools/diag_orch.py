"""体检编排器:逐 bench 起垫片、perf attach 稳态窗、符号分桶、汇总。"""
import sys, os, json, subprocess, time, re

sys.path.insert(0, "/src/docs/dryrun")
import run_ab_full113 as ab

BENCHES = sys.argv[1].split(",")
PY = "/opt/python/cp311-cp311/bin/python3.11"
ENV = dict(os.environ)
ENV.update({
    "DIFFGATE_JIT": "1", "PYTHONJITAUTO": "4", "PYTHONHASHSEED": "0",
    "PYTHONPATH": "/tmp/m9sc:/tmp/ppdeps:/src/scratch/lib.linux-aarch64-cpython-311:/src/cinderx/PythonLib",
})

BUCKETS = [
    ("compiler", re.compile(r"jit::(lir|hir|codegen)|LinearScan|LiveInterval|DataflowAnalysis|asmjit|eliminateDeadCode")),
    ("interp", re.compile(r"Ci_EvalFrameDefault_311")),
    ("ic_helper", re.compile(r"LoadAttrCache|LoadMethodCache|StoreAttrCache|LoadTypeMethodCache|ci_hinted|ci_peek|getDictKeysIndex|DescrOrClassVar|SplitMutator|CombinedMutator")),
    ("jit_rt", re.compile(r"JITRT_|jitVectorcall|Ci_Stock")),
]

def classify(dso, sym):
    for name, rx in BUCKETS:
        if rx.search(sym):
            return name
    if dso.startswith("[") or "anon" in dso or dso == "[unknown]" or dso.startswith("perf-"):
        return "jit_code"
    if "unknown" in sym and "python" not in dso and "_cinderx" not in dso and not dso.startswith("lib"):
        return "jit_code"
    if dso == "python3.11":
        return "cpython_c"
    if dso == "_cinderx.so":
        return "cinderx_other"
    if dso.startswith("[kernel"):
        return "kernel"
    return "libs"

jobs = ab.load_jobs()
rows = {}
for b in BENCHES:
    if b not in jobs:
        rows[b] = {"error": "no-such-job"}
        continue
    bdir, extra = jobs[b]
    out = f"/tmp/diag-{b}.json"
    ready = out + ".ready"
    for f in (out, ready):
        if os.path.exists(f):
            os.unlink(f)
    proc = subprocess.Popen(
        [PY, "/tmp/diag_shim.py", os.path.join(ab.BM_ROOT, bdir), out] + list(extra),
        env=ENV, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    t0 = time.time()
    while not os.path.exists(ready) and time.time() - t0 < 120 and proc.poll() is None:
        time.sleep(0.3)
    if not os.path.exists(ready):
        err = proc.stderr.read()[-300:].decode(errors="replace") if proc.poll() is not None else "warmup-timeout"
        proc.kill()
        rows[b] = {"error": err}
        continue
    pid = open(ready).read().strip()
    pdata = f"/tmp/diag-{b}.perf"
    subprocess.run(["perf", "record", "-F", "499", "-p", pid, "-o", pdata, "--", "sleep", "8"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc.wait(timeout=300)
    rep = subprocess.run(["perf", "report", "--stdio", "-i", pdata, "-F", "overhead,dso,sym", "--percent-limit", "0.3"],
                         capture_output=True, text=True).stdout
    shares = {}
    for line in rep.splitlines():
        m = re.match(r"\s*([\d.]+)%\s+(\S+)\s+(.*)", line)
        if not m:
            continue
        pct, dso, sym = float(m.group(1)), m.group(2), m.group(3).strip()
        cat = classify(dso, sym)
        shares[cat] = shares.get(cat, 0.0) + pct
    row = json.load(open(out)) if os.path.exists(out) else {"error": "no-stats"}
    row["shares"] = {k: round(v, 1) for k, v in sorted(shares.items(), key=lambda kv: -kv[1])}
    rows[b] = row
    print(b, "done", flush=True)
json.dump(rows, open("/tmp/diag-middle28.json", "w"), indent=1)
print("ALL_DONE")
