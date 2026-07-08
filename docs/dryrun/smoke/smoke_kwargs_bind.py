def f1(a, b=2, *, c=3, **kw): return (a, b, c, sorted(kw.items()))
def f2(a, *args, b=9): return (a, args, b)
def drive():
    assert f1(1) == (1, 2, 3, [])
    assert f1(1, c=7, x=8) == (1, 2, 7, [("x", 8)])
    assert f1(1, 5, c=6, y=1, z=2) == (1, 5, 6, [("y", 1), ("z", 2)])
    assert f2(1, 2, 3, b=4) == (1, (2, 3), 4)
    try:
        f1(1, q=1, b=99, c=98)
    except TypeError:
        pass
    assert f1(1, b=99, c=98) == (1, 99, 98, [])
for _ in range(200):
    drive()
from cinderx import jit
assert {"f1", "f2"} <= {f.__name__ for f in jit.get_compiled_functions()}
print("kwargs 语义 OK")
