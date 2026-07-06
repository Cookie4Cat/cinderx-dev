import importlib.util, sys, time
BM = "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks"

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod

which = sys.argv[1]
if which == "generators":
    mod = load("gen_bm", f"{BM}/bm_generators/run_benchmark.py")
    for _ in range(3): mod.bench_generators(1)
    best = min(mod.bench_generators(1) for _ in range(8))
    print(f"generators best {best*1000:.2f} ms")
elif which == "go":
    mod = load("go_bm", f"{BM}/bm_go/run_benchmark.py")
    for _ in range(2): mod.versus_cpu()
    best = min(mod.versus_cpu() for _ in range(6))
    print(f"go best {best*1000:.1f} ms")
elif which == "pickle":
    mod = load("pk_bm", f"{BM}/bm_pickle/run_benchmark.py")
    sys.modules["_pickle"] = None
    import pickle as pure
    class Opt: pass
    o = Opt(); o.pure_python = True; o.protocol = 4
    mod.bench_unpickle(2, pure, o)
    best = min(mod.bench_unpickle(2, pure, o) for _ in range(6))
    print(f"unpickle best {best*1000:.2f} ms")
