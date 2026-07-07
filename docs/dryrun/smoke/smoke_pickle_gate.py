# _Unpickler.load 形态:try 内无限循环,唯一 return 在异常 handler 内
class _Stop(Exception):
    def __init__(self, value): self.value = value
def spine(items):
    it = iter(items)
    acc = []
    try:
        while True:
            v = next(it)
            if v is None:
                raise _Stop(acc)
            acc.append(v * 2)
    except _Stop as s:
        return s.value
data = [1, 2, 3, None]
want = [2, 4, 6]
for _ in range(100):
    assert spine(data) == want
from cinderx import jit
names = {f.__name__ for f in jit.get_compiled_functions()}
assert "spine" in names, f"spine 未编译: {names}"
# 只以 raise 出口的函数
def always_raises(x):
    raise ValueError(x)
ok = 0
for i in range(60):
    try: always_raises(i)
    except ValueError: ok += 1
assert ok == 60
assert "always_raises" in {f.__name__ for f in jit.get_compiled_functions()}
# 真身:pickle 纯 Python 脊柱
import io, importlib
import pickle as _p
py_pickle = importlib.import_module("pickle")
Unpickler = py_pickle._Unpickler
buf = _p.dumps({"a": [1, 2, {"b": (3, 4)}], "c": "xyz"})
for _ in range(80):
    got = Unpickler(io.BytesIO(buf)).load()
    assert got == {"a": [1, 2, {"b": (3, 4)}], "c": "xyz"}
compiled = {f.__qualname__ for f in jit.get_compiled_functions()}
assert "_Unpickler.load" in compiled, sorted(c for c in compiled if "load" in c.lower())
print("pickle-gate 语义 OK, _Unpickler.load 已编译")
