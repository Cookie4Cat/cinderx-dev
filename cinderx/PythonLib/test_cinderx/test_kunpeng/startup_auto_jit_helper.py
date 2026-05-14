import sys


def auto_jit_target(value):
    return value + 1


result = None
for value in range(5):
    result = auto_jit_target(value)

cinderx = sys.modules.get("cinderx")
cinderjit = sys.modules.get("cinderjit")

assert cinderx is not None, "cinderx was not auto-loaded"
assert cinderjit is not None, "cinderjit was not auto-loaded"
assert cinderx.is_initialized(), "cinderx was not initialized"
assert cinderjit.is_enabled(), "cinderx JIT is not enabled"
assert cinderjit.is_jit_compiled(auto_jit_target), "auto_jit_target was not compiled"
assert cinderjit.get_compiled_size(auto_jit_target) > 0
assert result == 5
