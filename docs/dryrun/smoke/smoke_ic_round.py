import sys, math
import cinderx; cinderx.init()
import cinderjit

# --- store 三态：覆写 / 插入 / 删除后重插 ---
class P:
    def __init__(self):
        self.a = 1
def poke(o, v):
    o.a = v
    o.b = v + 1        # 插入（首次）后覆写
    return o.a + o.b
objs = [P() for _ in range(4)]
for _ in range(50):
    for o in objs: poke(o, 3)
cinderjit.force_compile(poke)
assert poke(objs[0], 10) == 21
del objs[0].b
assert poke(objs[0], 5) == 11          # 删除后重插入
objs[0].__dict__["z"] = 9              # 物化
assert poke(objs[0], 7) == 15
assert objs[0].z == 9
# 引用计数平衡：覆写同一 sentinel 多次
SENT = object()
import gc; gc.collect()
class Q:
    def __init__(self): self.s = None
q = Q()
def sets(o, v): o.s = v
for _ in range(50): sets(q, SENT)
cinderjit.force_compile(sets)
base = sys.getrefcount(SENT)
for _ in range(10000): sets(q, SENT)
gc.collect()
assert sys.getrefcount(SENT) == base, sys.getrefcount(SENT) - base
sets(q, None)
assert sys.getrefcount(SENT) == base - 1

# --- 模块属性站点扩展：命中 + 变异失效 ---
def usesqrt(x):
    return math.sqrt(x)
for _ in range(50): usesqrt(4.0)
cinderjit.force_compile(usesqrt)
assert usesqrt(9.0) == 3.0
orig = math.sqrt
math.sqrt = lambda x: 42.0             # 模块 dict 变异 → 版本失效
assert usesqrt(9.0) == 42.0
math.sqrt = orig
assert usesqrt(16.0) == 4.0
del math.sqrt                          # 删除 → 泛型路径 AttributeError? math.sqrt 是 builtin...
math.sqrt = orig
assert usesqrt(25.0) == 5.0

# --- 类属性站点扩展：命中 + 类变异失效 + 继承层变异 ---
class Base:
    K = 100
class Sub(Base): pass
def readk(cls):
    return cls.K
for _ in range(50): readk(Sub)
cinderjit.force_compile(readk)
assert readk(Sub) == 100
Base.K = 200                           # MRO 上层变异 → PyType_Modified 传播
assert readk(Sub) == 200
Sub.K = 300                            # 本类遮蔽
assert readk(Sub) == 300
del Sub.K
assert readk(Sub) == 200
# 描述符形态拒缓存：classmethod 每次语义
class D:
    @classmethod
    def cm(cls): return cls.__name__
def readcm(c): return c.cm()
for _ in range(50): readcm(D)
cinderjit.force_compile(readcm)
assert readcm(D) == "D"
print("IC round smoke OK")
