#!/usr/bin/env python3
"""M6 recursion-depth smoke (dry-run).

Three probes, each in a fresh subprocess:
  A. interp baseline: deep self-recursion -> expect RecursionError
  B. jit force-compiled self-recursion   -> expect RecursionError,
     suspect SEGV (JIT has no recursion check on any version)
  C. mirror consistency: after JIT'd calls + deopts, an interpreted
     recursive probe must still raise RecursionError at the configured
     limit (recursion_remaining not corrupted by JIT'd frames)

Usage: recursion_smoke.py <python> (runs subprocesses with/without JIT env)
"""

import subprocess
import sys
import textwrap

PY = sys.argv[1] if len(sys.argv) > 1 else sys.executable

CASE_A = """
import sys
sys.setrecursionlimit(3000)
def f(n):
    return 0 if n == 0 else 1 + f(n - 1)
try:
    f(100000)
    print("A:NO-ERROR")
except RecursionError:
    print("A:RECURSIONERROR")
"""

CASE_B = """
import sys, cinderx
cinderx.init()
import cinderjit
sys.setrecursionlimit(3000)
def f(n):
    return 0 if n == 0 else 1 + f(n - 1)
f(5)  # warm
cinderjit.force_compile(f)
assert cinderjit.is_jit_compiled(f), "f not compiled"
try:
    f(100000)
    print("B:NO-ERROR")
except RecursionError:
    print("B:RECURSIONERROR")
"""

CASE_C = """
import sys, cinderx
cinderx.init()
import cinderjit
sys.setrecursionlimit(200)

def g(x):
    # attribute miss forces a deopt-ish error path; swallowing keeps going
    try:
        return x.missing
    except AttributeError:
        return None

class O: pass
g(O())
cinderjit.force_compile(g)
for _ in range(50):
    g(O())  # JIT'd calls incl. exception path

# interpreted probe: must still hit RecursionError near limit 200
def probe(n):
    return 0 if n == 0 else 1 + probe(n - 1)
try:
    probe(10000)
    print("C:NO-ERROR")
except RecursionError:
    # measure achievable depth to detect drift
    lo, hi = 1, 400
    best = 0
    while lo <= hi:
        mid = (lo + hi) // 2
        try:
            probe(mid)
            best = mid
            lo = mid + 1
        except RecursionError:
            hi = mid - 1
    print(f"C:RECURSIONERROR depth~{best} limit=200")
"""


def run(tag, code, env_extra):
    import os

    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.run(
        [PY, "-c", textwrap.dedent(code)],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )
    out = p.stdout.strip() or "(no stdout)"
    sig = f" rc={p.returncode}"
    if p.returncode < 0:
        sig += f" SIGNAL={-p.returncode}"
    err = p.stderr.strip().splitlines()
    tail = f" | stderr: {err[-1]}" if err else ""
    print(f"[{tag}] {out}{sig}{tail}")


jit_env = {"PYTHONJIT": "1"}
run("A interp", CASE_A, {})
run("B jit", CASE_B, jit_env)
run("C mirror", CASE_C, jit_env)
