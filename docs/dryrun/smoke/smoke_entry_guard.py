import cinderx; cinderx.init()
import cinderjit, sys

def deep(n):
    if n == 0:
        return 0
    return 1 + deep(n - 1)

def raises(n):
    if n > 5:
        raise ValueError("x")
    return n

def kwf(a, b=2, c=3):
    return a + b + c

class T:
    def __init__(self):
        self.v = 1

def attr_reader(o):
    return o.v

for f in (deep, raises, kwf, attr_reader):
    cinderjit.force_compile(f)

def probe():
    def r(n):
        try:
            return r(n + 1)
        except RecursionError:
            return n
    return r(0)

# ① 编译递归深度上限:必须 RecursionError 而非崩溃/深度错乱
try:
    deep(100000)
    raise AssertionError("expected RecursionError")
except RecursionError:
    pass
assert deep(400) == 400
# ② 账本平衡:大量异常/绑参重入/慢路径后解释探针深度不漂移
base = probe()
for i in range(3000):
    try:
        raises(10)
    except ValueError:
        pass
for i in range(3000):
    assert kwf(1) == 6
    assert kwf(1, 5) == 9
    assert kwf(1, c=7) == 10   # kwargs → JITRT_CallWithKeywordArgs 重入
    assert kwf(*[1, 2, 3]) == 6
try:
    kwf(1, 2, 3, 4)
    raise AssertionError("expected TypeError")
except TypeError:  # 绑参失败路径(无 Enter 无 Leave)
    pass
after = probe()
assert after == base, (base, after)
# ③ deopt 补账:类型突变触发 guard 失败 deopt 后深度不漂移
o = T()
for i in range(100):
    assert attr_reader(o) == 1
class T2:
    v = 99
for i in range(50):
    assert attr_reader(T2()) == 99  # 多态/失效路径
after2 = probe()
assert after2 == base, (base, after2)
# ④ tracing:settrace 激活时编译函数照发事件(入口分流)
events = []
def tr(frame, event, arg):
    if frame.f_code.co_name == "kwf":
        events.append(event)
    return tr
sys.settrace(tr)
kwf(1)
sys.settrace(None)
assert "call" in events and "return" in events, events
after3 = probe()
assert after3 == base, (base, after3)
print("entry-guard smoke OK; probe depth stable at", base)
