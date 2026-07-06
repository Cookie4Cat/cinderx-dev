import importlib.util, sys, json
BM = "/opt/python/cp311-cp311/lib/python3.11/site-packages/pyperformance/data-files/benchmarks"

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod

which = sys.argv[1]
if which == "richards":
    mod = load("richards_bm", f"{BM}/bm_richards/run_benchmark.py")
    r = mod.Richards()
    warm = lambda: r.run(3)
    steady = lambda: r.run(10)
elif which == "deltablue":
    mod = load("deltablue_bm", f"{BM}/bm_deltablue/run_benchmark.py")
    warm = lambda: [mod.delta_blue(30) for _ in range(3)]
    steady = lambda: [mod.delta_blue(100) for _ in range(5)]
elif which == "raytrace":
    mod = load("raytrace_bm", f"{BM}/bm_raytrace/run_benchmark.py")
    warm = lambda: mod.bench_raytrace(1, 60, 60, None)
    steady = lambda: mod.bench_raytrace(4, 100, 100, None)
elif which == "hexiom":
    mod = load("hexiom_bm", f"{BM}/bm_hexiom/run_benchmark.py")
    warm = lambda: mod.main(1, 25)
    steady = lambda: mod.main(4, 25)

warm(); warm()
import cinderjit
cinderjit.get_and_clear_inline_cache_stats()   # 归零，只留稳态窗口
steady()
stats = cinderjit.get_and_clear_inline_cache_stats()
g = stats["globals"]
g["_bench"] = which
sites = stats["la_slow_sites"]
g["_sites"] = dict(sorted(sites.items(), key=lambda kv: -kv[1])[:10])
print(json.dumps(g))
