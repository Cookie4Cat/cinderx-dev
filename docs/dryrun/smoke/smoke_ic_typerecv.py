# IC 快路径轮（C1/C2）语义冒烟：类型接收者缓存（多态 attr/元类型数据
# 描述符/类变量失效/类型受者方法）与 __getattr__ 类命中侧缓存放行
# （实例遮蔽/类热补丁/删除回落）。双模一致性要求同 smoke_m10_fixes。
# 运行：PYTHONPATH=<sitecustomize:cinderx> PYTHONJITAUTO=2 python3.11 本文件

import sys

def check(cond, msg):
    if not cond:
        print("FAIL:", msg); sys.exit(1)

# --- C1: 类型受者多态 attr（pprint 形态）---
def type_repr(o):
    return type(o).__repr__

def drive_type_attr():
    seen = set()
    for o in (1, "s", [], {}, (), 3.5, set()):
        for _ in range(30):
            seen.add(type_repr(o))
    return seen

for _ in range(6):
    reprs = drive_type_attr()
check(type_repr(1) is int.__repr__, "int.__repr__ identity")
check(type_repr("x") is str.__repr__, "str.__repr__ identity")

# --- C1: type.__name__（元类型数据描述符形态）---
def type_name(o):
    return type(o).__name__

for _ in range(6):
    names = [type_name(o) for o in (1, "s", [], {})]
check(names == ["int", "str", "list", "dict"], "type names %r" % names)

# --- C1: 类变量与版本失效 ---
class K:
    marker = "v1"

def read_k(t):
    return t.marker

for _ in range(200):
    check(read_k(K) == "v1", "classvar v1")
K.marker = "v2"   # PyType_Modified -> 版本前移，条目须失效
check(read_k(K) == "v2", "classvar invalidation")

# --- C1: 类型受者方法（lm 委托）---
class M:
    @classmethod
    def cm(cls, x):
        return (cls, x)
    @staticmethod
    def sm(x):
        return x * 2
    def plain(self):
        return 42

def call_type_methods(t):
    a = t.cm(1)
    b = t.sm(3)
    f = t.plain
    return a, b, f

for _ in range(200):
    a, b, f = call_type_methods(M)
check(a == (M, 1), "classmethod via type recv")
check(b == 6, "staticmethod via type recv")
check(f is M.plain, "plain function identity on class")
check(M.cm(5) == (M, 5), "classmethod args")

# --- C2: __getattr__ 类 ---
class Proxy:
    def __init__(self):
        self.real = "instance-attr"
        self._n = 0
    def method(self):
        return "cls-method"
    def __getattr__(self, name):
        return "dyn:" + name

def drive_proxy(p):
    r1 = p.real          # 实例属性（通用阶段命中，可缓存）
    r2 = p.method()      # 类方法（lm 溯源回填）
    r3 = p.ghost         # __getattr__ 兜底（不可缓存）
    return r1, r2, r3

p = Proxy()
for _ in range(300):
    r = drive_proxy(p)
check(r == ("instance-attr", "cls-method", "dyn:ghost"), "proxy semantics %r" % (r,))

# 遮蔽：实例属性覆盖类方法
p2 = Proxy()
for _ in range(50):
    drive_proxy(p2)
p2.method = lambda: "shadowed"
check(p2.method() == "shadowed", "instance shadow after cache")

# 类变更失效
Proxy.method = lambda self: "patched"
check(p.method() == "patched", "class mutation invalidation")

# 删除实例属性后回落 __getattr__
del p.real
check(p.real == "dyn:real", "deleted attr falls to __getattr__")

# --- C2 回归卫兵：__getattr__ 惰性填充 + 新实例空槽 miss ---
# sphinx Config 模式：属性由 __getattr__ 首访时写入实例字典。缓存条目
# 填充后，新实例的空槽"确定 miss"必须回落 __getattr__ 兜底而非直接抛
# AttributeError（实测教训：sphinx 全部构建瞬间失败）。
class LazyCfg:
    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        val = "lazy:" + name
        self.__dict__[name] = val
        return val

def read_lang(c):
    return c.language

for i in range(100):
    v = read_lang(LazyCfg())   # 每次全新实例：空槽 miss
    check(v == "lazy:language", "lazy population iter %d got %r" % (i, v))

# 未知属性仍须正确抛 AttributeError（经 __getattr__ 内的 raise）
try:
    LazyCfg()._missing
    check(False, "expected AttributeError")
except AttributeError:
    pass

print("ALL PASS")

try:
    import cinderjit
except ImportError:
    raise SystemExit(0)
import cinderjit
stats = cinderjit.get_and_clear_inline_cache_stats()["globals"]
print({k: v for k, v in stats.items() if v and ("site_type" in k or k in ("la_invoke","la_slow","lm_helper","lm_slow"))})
