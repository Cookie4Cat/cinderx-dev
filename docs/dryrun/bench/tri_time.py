import importlib.util, sys, time
BM = "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks"
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod
db = load("db", f"{BM}/bm_deltablue/run_benchmark.py")
rt = load("rt", f"{BM}/bm_raytrace/run_benchmark.py")
for _ in range(8): db.delta_blue(30)
best = 1e9
for _ in range(5):
    t0 = time.perf_counter(); db.delta_blue(100); best = min(best, time.perf_counter()-t0)
print(f"deltablue(100) best {best*1000:.2f} ms")
rt.bench_raytrace(1, 60, 60, None)
best = 1e9
for _ in range(4):
    t0 = time.perf_counter(); rt.bench_raytrace(1, 100, 100, None); best = min(best, time.perf_counter()-t0)
print(f"raytrace(100x100) best {best*1000:.2f} ms")
