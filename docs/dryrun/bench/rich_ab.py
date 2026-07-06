import importlib.util, sys, time
spec = importlib.util.spec_from_file_location("richards_bm", "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks/bm_richards/run_benchmark.py")
mod = importlib.util.module_from_spec(spec); sys.modules["richards_bm"] = mod
spec.loader.exec_module(mod)
r = mod.Richards()
r.run(3)
best = 1e9
for _ in range(5):
    t0 = time.perf_counter(); r.run(10); dt = (time.perf_counter()-t0)/10
    best = min(best, dt)
print(f"per-iter {best*1000:.2f} ms")
