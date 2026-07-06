import importlib.util, sys, json
BM = "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks"

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod

which = sys.argv[1]
if which == "go":
    mod = load("go_bm", f"{BM}/bm_go/run_benchmark.py")
    warm = lambda: mod.versus_cpu()
    steady = lambda: [mod.versus_cpu() for _ in range(3)]
elif which == "generators":
    mod = load("gen_bm", f"{BM}/bm_generators/run_benchmark.py")
    warm = lambda: mod.bench_generators(1)
    steady = lambda: mod.bench_generators(5)
elif which == "unpickle":
    mod = load("pk_bm", f"{BM}/bm_pickle/run_benchmark.py")
    sys.modules["_pickle"] = None      # 屏蔽 C 加速，落纯 Python pickle
    import pickle as pure
    assert not hasattr(pure.Unpickler, "__module__") or pure.Unpickler.__module__ == "pickle" 
    class Opt: pass
    o = Opt(); o.pure_python = True; o.protocol = 4
    # bench_unpickle 需要 pure-python pickle:复制 main 的处理
    warm = lambda: mod.bench_unpickle(2, pure, o)
    steady = lambda: mod.bench_unpickle(8, pure, o)

warm(); warm()
import cinderjit
cinderjit.get_and_clear_inline_cache_stats()
steady()
stats = cinderjit.get_and_clear_inline_cache_stats()
g = stats["globals"]
g["_bench"] = which
g["_sites"] = dict(sorted(stats["la_slow_sites"].items(), key=lambda kv: -kv[1])[:8])
print(json.dumps(g))
