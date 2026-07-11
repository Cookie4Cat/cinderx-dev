"""中间带体检垫片:伪 Runner 截获基准函数,进程内暖机+稳态窗+JIT 统计倾倒。
用法: diag_shim.py <bench_dir> <out_json> [extra_opts...]
协议: 暖机(自适应,≥2s)→写 <out>.ready(含 pid)→sleep 2(供 perf attach)
      →稳态跑 10s →倾倒统计到 out_json。
"""
import sys, os, json, time, runpy, asyncio

BENCH_DIR, OUT = sys.argv[1], sys.argv[2]
EXTRA = sys.argv[3:]
CAPTURED = []

import pyperf

class FakeRunner:
    def __init__(self, *a, **k):
        import argparse
        self.argparser = argparse.ArgumentParser()
        self.metadata = k.get("metadata", {})
        self.args = None
    def parse_args(self, args=None):
        ns, _ = self.argparser.parse_known_args(EXTRA)
        self.args = ns
        return ns
    def bench_time_func(self, name, func, *args, **kw):
        CAPTURED.append(("time", name, func, args))
        return None
    def bench_func(self, name, func, *args, **kw):
        CAPTURED.append(("func", name, func, args))
        return None
    def bench_async_func(self, name, func, *args, **kw):
        CAPTURED.append(("async", name, func, args))
        return None
    def bench_command(self, name, cmd, **kw):
        CAPTURED.append(("command", name, None, None))
        return None

pyperf.Runner = FakeRunner
sys.argv = ["run_benchmark.py"] + EXTRA
sys.path.insert(0, BENCH_DIR)

runpy.run_path(os.path.join(BENCH_DIR, "run_benchmark.py"), run_name="__main__")

if not CAPTURED or CAPTURED[0][0] == "command":
    json.dump({"error": "unsupported-shape", "shapes": [c[0] for c in CAPTURED]}, open(OUT, "w"))
    sys.exit(3)

kind, name, func, args = CAPTURED[0]

def run_once():
    if kind == "time":
        return func(1, *args)
    if kind == "func":
        func(*args)
        return None
    if kind == "async":
        asyncio.run(func(*args))
        return None

def run_for(seconds):
    n = 0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        run_once()
        n += 1
    return n

warm_n = run_for(3.0)          # 暖机:越过 auto=4 阈值并完成编译
import cinderjit
cinderjit.get_and_clear_runtime_stats()
open(OUT + ".ready", "w").write(str(os.getpid()))
time.sleep(2.0)                # perf attach 窗口
steady_n = run_for(10.0)       # 稳态(perf 采样期)
stats = cinderjit.get_and_clear_runtime_stats()
from collections import Counter
c = Counter()
for ev in stats.get("deopt", []):
    n_ = ev.get("normal", ev)
    c[(n_.get("reason"), n_.get("description"), n_.get("func_qualname"))] += ev.get("count", 1)
fns = cinderjit.get_compiled_functions()
json.dump({
    "bench_kind": kind, "warm_iters": warm_n, "steady_iters": steady_n,
    "compiled": len(fns),
    "jit_code_kb": sum(cinderjit.get_compiled_size(f) for f in fns) // 1024,
    "deopt_total": sum(c.values()),
    "deopt_top": [
        {"reason": r, "descr": d, "func": q, "n": n_}
        for (r, d, q), n_ in c.most_common(5)
    ],
}, open(OUT, "w"), indent=1)
