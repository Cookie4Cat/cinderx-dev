import sys
import cinderx; cinderx.init()
import cinderjit

# --- 物化实例:读/写/hint 失效再定位/删除语义 ---
class Fat:
    def __init__(self):
        for i in range(12):
            setattr(self, f"a{i}", i)
objs = [Fat() for _ in range(4)]
for o in objs:
    o.__dict__          # 物化
def rd(o): return o.a3
def wr(o, v): o.a3 = v
for _ in range(60):
    for o in objs: rd(o); wr(o, 5)
for f in (rd, wr): cinderjit.force_compile(f)
assert rd(objs[0]) == 5
wr(objs[0], 77); assert rd(objs[0]) == 77
del objs[0].a3
try:
    rd(objs[0]); assert False
except AttributeError: pass
objs[0].a3 = 9; assert rd(objs[0]) == 9
# 字典重排(触发 combined/hint 失效):插入新键
objs[0].zz = 1; assert rd(objs[0]) == 9
# 引用计数平衡
SENT = object()
import gc; gc.collect()
wr(objs[1], SENT)
base = sys.getrefcount(SENT)
for _ in range(20000): wr(objs[1], SENT)
gc.collect()
assert sys.getrefcount(SENT) == base, sys.getrefcount(SENT) - base

# --- 实例属性方法位(unpickle 形态):双形态+删除+类侧变化 ---
class Reader:
    def __init__(self, fn):
        self.read = fn
def drive(r): return r.read()
rs = [Reader(lambda: 1), Reader(lambda: 2)]
for _ in range(60):
    for r in rs: drive(r)
cinderjit.force_compile(drive)
assert drive(rs[0]) == 1 and drive(rs[1]) == 2
rs[0].read = lambda: 42
assert drive(rs[0]) == 42
del rs[0].read
try:
    drive(rs[0]); assert False
except AttributeError: pass
rs[0].read = lambda: 7; assert drive(rs[0]) == 7
Reader.read = lambda self: 99          # 类侧新增(非数据描述符,实例优先)
assert drive(rs[1]) == 2
del rs[1].read                         # 实例删除后落类侧
assert drive(rs[1]) == 99

# --- 共享键成长驱逐:分批加属性的类,方法缓存自愈 ---
class Grow:
    def m(self): return 1
def callm(o): return o.m()
g1 = Grow(); g1.x = 1
for _ in range(40): callm(g1)
cinderjit.force_compile(callm)
assert callm(g1) == 1
g2 = Grow(); g2.x = 1; g2.y = 2; g2.z = 3   # 共享键成长 → 旧条目过期
for _ in range(10):
    assert callm(g2) == 1 and callm(g1) == 1
print("laggards smoke OK")
