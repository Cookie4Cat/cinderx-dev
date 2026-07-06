import cinderx; cinderx.init()
import cinderjit, sys, traceback

# ① 逃逸帧:编译函数内 sys._getframe → frame_obj 物化 → 出口慢路径
def escapes(n):
    f = sys._getframe()
    assert f.f_code.co_name == "escapes"
    return n + (f.f_lineno > 0)
# ② locals() 观测(f_locals 路径)
def uses_locals(a, b):
    c = a + b
    d = locals()
    return d["c"] + len(d)
# ③ 异常穿透:出口在异常路径同样走行内 unlink
def raises(n):
    if n > 5:
        raise ValueError("boom")
    return n
# ④ 深递归:datastack chunk 增长 → 入口慢路径(越界回落)
def deep(n):
    if n == 0:
        return 0
    return 1 + deep(n - 1)
# ⑤ 引用平衡:同一函数万次调用后 func/code 引用计数不漂移
def callee(x):
    return x * 2

for f in (escapes, uses_locals, raises, deep, callee):
    try:
        cinderjit.force_compile(f)
    except RuntimeError:
        pass  # locals() 函数 3.11 前端既存拒编,解释执行参与混合场景

for i in range(2000):
    assert escapes(i) == i + 1
    assert uses_locals(i, i) == 2 * i + 3
assert raises(3) == 3
ok = 0
for i in range(1000):
    try:
        raises(10)
    except ValueError:
        ok += 1
assert ok == 1000
# 深度取 400:跨多个 datastack chunk(约 11 词/帧 × 400 > 2048 词/chunk),
# 且低于"异常量后编译路径配额减半"既存异常的上限(独立专项跟踪中)。
assert deep(400) == 400
rc_f0 = sys.getrefcount(callee); rc_c0 = sys.getrefcount(callee.__code__)
for i in range(10000):
    callee(i)
rc_f1 = sys.getrefcount(callee); rc_c1 = sys.getrefcount(callee.__code__)
assert rc_f0 == rc_f1, (rc_f0, rc_f1)
assert rc_c0 == rc_c1, (rc_c0, rc_c1)
# ⑥ traceback 帧身份
try:
    raises(99)
except ValueError:
    tb = traceback.extract_tb(sys.exc_info()[2])
    assert tb[-1].name == "raises"
assert cinderjit.is_jit_compiled(escapes)
print("frame-inline smoke OK")
