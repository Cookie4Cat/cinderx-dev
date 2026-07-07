# 泛型调用位点 × 各类可调用体(被调方类型对编译器不可见)
class WithCall:
    def __call__(self, x): return x + 7
class Meta(type): pass
def pyfunc(x): return x + 1
def dispatch(f, x):  # f 类型不可见 → 泛型 VectorCall
    return f(x)
import functools
callables = [
    (pyfunc, 5, 6),
    (len, [1,2,3], 3),                      # C 函数
    (WithCall(), 5, 12),                    # __call__ 实例
    ("abc".index, "b", 1),                  # 内建绑定方法
    (functools.partial(pyfunc), 5, 6),      # partial 对象
    (int, "42", 42),                        # 类型调用
]
for _ in range(200):
    for f, arg, want in callables:
        got = dispatch(f, arg)
        assert got == want, (f, got, want)
# 异常传播
def boom(x): raise ValueError("x")
ok = 0
for _ in range(50):
    try:
        dispatch(boom, 1)
    except ValueError:
        ok += 1
assert ok == 50
# kwargs 经泛型位点
def kwf(a, *, b=2): return a * b
def dispatch_kw(f, a, b):
    return f(a, b=b)
for _ in range(100):
    assert dispatch_kw(kwf, 3, 4) == 12
from cinderx import jit
names = {f.__name__ for f in jit.get_compiled_functions()}
assert "dispatch" in names and "dispatch_kw" in names, names
print("call-dispatch 语义 OK")
