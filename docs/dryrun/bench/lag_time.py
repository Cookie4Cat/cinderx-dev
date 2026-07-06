import importlib.util, sys, time
BM = "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks"
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod
which = sys.argv[1]
if which == "go":
    mod = load("go_bm", f"{BM}/bm_go/run_benchmark.py")
    mod.versus_cpu(); mod.versus_cpu()
    best = 1e9
    for _ in range(5):
        t0 = time.perf_counter(); mod.versus_cpu(); best = min(best, time.perf_counter()-t0)
    print(f"go best {best*1000:.1f} ms")
elif which == "unpickle":
    mod = load("pk_bm", f"{BM}/bm_pickle/run_benchmark.py")
    sys.modules["_pickle"] = None
    import pickle as pure
    class Opt: pass
    o = Opt(); o.pure_python = True; o.protocol = 4
    mod.bench_unpickle(2, pure, o)
    best = 1e9
    for _ in range(5):
        best = min(best, mod.bench_unpickle(4, pure, o) / 4)
    print(f"unpickle per-loop best {best*1000:.2f} ms")
