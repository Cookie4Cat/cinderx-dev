import os

import cinderx.jit as jit


def helper(value):
    return value


def fresh_probe(tag):
    ns = {}
    exec(compile("def fresh(v):\n    return v\n", tag, "exec"), ns)
    fresh = ns["fresh"]
    for value in range(64):
        fresh(value)
    return jit.is_jit_compiled(fresh)


for value in range(5000):
    helper(value)
assert jit.is_jit_compiled(helper), "parent must release"

pid = os.fork()
if pid == 0:
    # Child must re-prove the budget; inherited compiled code stays.
    ok = not fresh_probe("<child>") and jit.is_jit_compiled(helper)
    os._exit(0 if ok else 17)

assert os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]) == 0
assert fresh_probe("<parent>"), "parent keeps its earned release"
