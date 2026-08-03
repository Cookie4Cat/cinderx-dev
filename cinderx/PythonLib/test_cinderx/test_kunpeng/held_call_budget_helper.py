import os

import cinderx.jit as jit


def helper(value):
    return value


for value in range(int(os.environ["CALLS"])):
    helper(value)

want = os.environ["WANT"] == "compiled"
got = jit.is_jit_compiled(helper)
assert got == want, (want, got, jit.count_interpreted_calls(helper))
