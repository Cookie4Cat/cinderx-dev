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
    open("/tmp/pmp_ready", "w").close()
    while True: mod.versus_cpu()
elif which == "generators":
    mod = load("gen_bm", f"{BM}/bm_generators/run_benchmark.py")
    mod.bench_generators(1)
    open("/tmp/pmp_ready", "w").close()
    while True: mod.bench_generators(3)
