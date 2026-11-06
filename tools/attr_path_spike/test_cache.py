"""Targeted invalidation/lifetime tests; no private host addresses or counter writes."""
import builtins
import gc
import json
import os
import sys
import threading
import types
import weakref
from pathlib import Path

import _key_probe as probe

arm = os.environ["TEST_ARM"]
cx = sys.modules.get("_cinderx")
checks = []


def check(name, condition):
    assert condition, name
    checks.append(name)


def make(body, namespace, name="f"):
    code = compile(f"def {name}():\n" + "\n".join(" " + line for line in body.splitlines()),
                   "<cache-test>", "exec")
    inner = next(x for x in code.co_consts if isinstance(x, types.CodeType))
    return types.FunctionType(inner, namespace)


def warm(fn):
    return probe.call_n(fn, 60)


if cx:
    s = cx._interp_getattr_stats311()
    check("jit_off", not any(cx._get_trigger_stats().values()))
    if arm in ("legacy", "fixed"):
        check("requested_mode", s["global_cache_enabled"] == (arm == "fixed"))

b = {"token": 7, "len": len, "range": range}
g = {"__builtins__": b}
f = make("return token", g)
check("builtin_initial", warm(f) == 7)
if arm == "fixed":
    before = cx._interp_getattr_stats311()["global_cache_hits"]
    warm(f)
    check("cache_hit_proven", cx._interp_getattr_stats311()["global_cache_hits"] >= before + 50)
    check("high_keys_supported", probe.snapshot(b)["version"] >= 2**31)
b["token"] = 9
check("builtin_value_rebind", f() == 9)
g["token"] = 13
check("global_shadow", f() == 13)
del g["token"]
check("unshadow", f() == 9)
del b["token"]
try:
    f()
except NameError:
    checks.append("missing_name_raises")
else:
    raise AssertionError("NameError lost")
b["token"] = 17
check("restore_builtin", warm(f) == 17)
for i in range(400):
    g["token"] = i
    assert f() == i
    del g["token"]
    b["token"] = -i
    assert f() == -i
checks.append("400_alternating_updates")
g.clear()
g.update(__builtins__=b, token=41)
check("globals_clear_update", f() == 41)
g |= {"token": 43}
check("globals_ior", f() == 43)
g.pop("token")
check("global_pop", f() == b["token"])
b.clear()
b.setdefault("token", 47)
check("builtins_clear_setdefault", f() == 47)
b.popitem()
try:
    f()
except NameError:
    checks.append("builtins_popitem")
else:
    raise AssertionError("stale value after popitem")
b["token"] = 53

# The shared code object must respect two different function builtins dicts.
g["__builtins__"] = b
f1 = types.FunctionType(f.__code__, g)
b2 = {"token": 59}
g["__builtins__"] = b2
f2 = types.FunctionType(f.__code__, g)
for _ in range(100):
    assert f1() == 53 and f2() == 59
checks.append("same_code_globals_different_builtins")
g2 = {"__builtins__": b2, "token": 61}
f3 = types.FunctionType(f.__code__, g2)
for _ in range(100):
    assert f1() == 53 and f3() == 61
checks.append("same_code_different_globals")

# CinderX STORE_GLOBAL uses the existing shared PEP509 content-version bridge.
writer = make("global token\ntoken = token + 1", g2)
warm(f3)
for i in range(200):
    writer()
    assert f3() == 62 + i
checks.append("cinderx_store_global_invalidates")

# A cached result is borrowed: removing its dict reference must release it.
class Value:
    pass

obj = Value()
g2["token"] = obj
assert warm(f3) is obj
ref = weakref.ref(obj)
del obj
del g2["token"]
gc.collect()
check("cached_value_not_retained", ref() is None)
check("deleted_value_falls_back_to_builtins", f3() == 59)

# Unicode-key-only cache guard must preserve arbitrary equality side effects.
class Key:
    def __hash__(self):
        return hash("token")
    def __eq__(self, other):
        equality_calls.append(other)
        return other == "token"

equality_calls = []
mixed_b = {Key(): 71}
mixed_f = make("return token", {"__builtins__": mixed_b})
check("mixed_keys_value", warm(mixed_f) == 71)
check("mixed_keys_no_cached_eq_bypass", len(equality_calls) == 60)

class BuiltinsDict(dict):
    def __getitem__(self, key):
        if key == "token":
            return 73
        return super().__getitem__(key)

sub_f = make("return token", {"__builtins__": BuiltinsDict(token=1)})
check("dict_subclass_getitem", warm(sub_f) == 73)

# Generator suspension and tracing may change namespaces between loads.
gg = {"__builtins__": builtins.__dict__, "token": 1}
gen_f = make("for _ in range(4):\n yield token", gg)
it = gen_f()
for i in range(4):
    gg["token"] = 101 + i
    assert next(it) == 101 + i
checks.append("generator_resume_invalidation")
trace_f = make("return token", gg)
trace_values = []

def trace(frame, event, arg):
    if frame.f_code is trace_f.__code__ and event == "call":
        gg["token"] = 200 + len(trace_values)
        trace_values.append(gg["token"])
    return trace

sys.settrace(trace)
try:
    got = [trace_f() for _ in range(5)]
finally:
    sys.settrace(None)
check("trace_mutation", got == [200, 201, 202, 203, 204])

# Bounded cache collisions, code destruction and namespace allocator reuse.
for i in range(2500):
    name = "dynamic_name_" + str(i)
    ns = {"__builtins__": {name: i}}
    fn = make("return " + name, ns)
    assert warm(fn) == i
    ns["__builtins__"][name] = -i
    assert fn() == -i
    if i % 50 == 0:
        gc.collect()
checks.append("2500_names_eviction_lifetime_churn")

# GIL-protected namespaces accessed from multiple threads; no cross-thread
# assumptions in a shared cache and no assertion across unsynchronized writes.
thread_errors = []
def thread_job(index):
    try:
        ns = {"__builtins__": {"token": index}}
        local_fn = make("return token", ns)
        for j in range(300):
            ns["__builtins__"]["token"] = index * 1000 + j
            assert local_fn() == index * 1000 + j
    except BaseException as error:
        thread_errors.append(repr(error))

threads = [threading.Thread(target=thread_job, args=(i,)) for i in range(4)]
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
check("four_thread_namespace_isolation", not thread_errors)

if hasattr(os, "fork"):
    pid = os.fork()
    if pid == 0:
        try:
            g2["token"] = 211
            assert warm(f3) == 211
        except BaseException:
            os._exit(1)
        os._exit(0)
    _, status = os.waitpid(pid, 0)
    check("fork_cache_copy_invalidation", status == 0)

if cx:
    for i in range(60):
        cx.remove_frame_evaluator()
        try:
            g2["token"] = i
            assert warm(f3) == i
        finally:
            cx.install_frame_evaluator()
        g2["token"] = -i
        assert warm(f3) == -i
    checks.append("stock_cinderx_alternation")
    check("jit_still_off", not any(cx._get_trigger_stats().values()))

record = {"arm": arm, "passed": True, "checks": len(checks), "check_names": checks,
          "python": sys.version, "runtime": cx._interp_getattr_stats311() if cx else None,
          "jit": cx._get_trigger_stats() if cx else None}
path = Path(os.environ.get("CACHE_TEST_OUTPUT", f"/work/validation/cache-{arm}.json"))
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text(json.dumps(record, indent=2))
print(json.dumps({"arm": arm, "checks": len(checks), "passed": True}))
