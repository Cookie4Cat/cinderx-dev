#!/usr/bin/env python3
"""M6 deopt state-recovery + settrace round-trip smoke (dry-run).

Each scenario runs twice in subprocesses: interp oracle vs jit (force
compiled, deopted mid-flight via cinderjit.force_uncompile). Output lines
must be identical between the two modes.

Scenarios:
  S1 locals+lineno at mid-function deopt (frame introspection from callee)
  S2 exception traceback through a deopted frame (line-level)
  S3 settrace round-trip: pre-installed tracer event stream
  S4 settrace installed mid-execution, then removed; JIT resumes after
"""

import os
import subprocess
import sys
import textwrap

PY = sys.argv[1] if len(sys.argv) > 1 else sys.executable

PRELUDE = """
import sys
MODE = sys.argv[1] if len(sys.argv) > 1 else "interp"
if MODE == "jit":
    import cinderx; cinderx.init()
    import cinderjit

def prep(*fns):
    if MODE != "jit":
        return
    for f in fns:
        f_args = getattr(f, "__smoke_warm_args__", None)
        cinderjit.force_compile(f)
        assert cinderjit.is_jit_compiled(f), f.__name__

def deopt(*fns):
    if MODE != "jit":
        return
    for f in fns:
        cinderjit.force_uncompile(f)
"""

S1 = PRELUDE + """
def probe():
    fr = sys._getframe(1)
    deopt(target)
    # read state through the materialized frame after entrypoint swap
    loc = {k: v for k, v in sorted(fr.f_locals.items())}
    print("S1", fr.f_lineno - target.__code__.co_firstlineno, loc)

def target(x):
    a = x + 1
    b = a * 2
    probe()
    c = a + b
    return c

prep(target)
print("S1 ret", target(10))
"""

S2 = PRELUDE + """
def target(x):
    a = x + 1
    if a > 5:
        raise ValueError(f"boom {a}")
    return a

prep(target)
import traceback
try:
    target(10)
except ValueError:
    tb = traceback.format_exc().splitlines()
    rel = [l.strip() for l in tb if "line" in l]
    base = target.__code__.co_firstlineno
    import re
    rel = [re.sub(r"line (\\d+)", lambda m: f"line+{int(m.group(1)) - base}", l)
           for l in rel]
    print("S2", rel[-1])
"""

S3 = PRELUDE + """
def target(x):
    a = x + 1
    b = a * 2
    return a + b

prep(target)
events = []
base = target.__code__.co_firstlineno
def tracer(frame, event, arg):
    if frame.f_code is target.__code__:
        events.append((event, frame.f_lineno - base))
    return tracer

sys.settrace(tracer)
r = target(3)
sys.settrace(None)
print("S3", r, events)
"""

S4 = PRELUDE + """
def install(events):
    base = target.__code__.co_firstlineno
    def tracer(frame, event, arg):
        if frame.f_code is target.__code__:
            events.append((event, frame.f_lineno - base))
        return tracer
    sys.settrace(tracer)

def target(x):
    a = x + 1
    install(EV)      # tracer appears mid-execution of a JIT'd frame
    b = a * 2
    return a + b

EV = []
prep(target)
r1 = target(3)
sys.settrace(None)
r2 = target(4)      # after removal: must run clean (and may re-enter JIT)
print("S4", r1, r2, EV)
"""


def run(tag, code, mode):
    env = dict(os.environ)
    p = subprocess.run(
        [PY, "-c", textwrap.dedent(code), mode],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )
    out = p.stdout.strip()
    if p.returncode != 0:
        err = p.stderr.strip().splitlines()
        out += f" [rc={p.returncode}{' SIG' + str(-p.returncode) if p.returncode < 0 else ''}]"
        if err:
            out += f" [stderr: {err[-1]}]"
    return out


failures = 0
for tag, code in [("S1", S1), ("S2", S2), ("S3", S3), ("S4", S4)]:
    a = run(tag, code, "interp")
    b = run(tag, code, "jit")
    status = "OK " if a == b else "DIFF"
    if a != b:
        failures += 1
    print(f"[{status}] {tag}")
    if a != b:
        print(f"    interp: {a}")
        print(f"    jit:    {b}")
    else:
        print(f"    both:   {a}")
print(f"failures={failures}")
sys.exit(1 if failures else 0)
