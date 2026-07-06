# C3 轮语义冒烟：la 桩 kind-7（kDescrOrClassVar）内联快路径。
# 覆盖：快形（名字不在共享键）/探测形（共享键内实例遮蔽）/后置遮蔽/
# 遮蔽删除/类级改写失效/property 与方法描述符留 helper/多态站点。
# 运行：PYTHONPATH=<sitecustomize:cinderx> PYTHONJITAUTO=2 python3.11 本文件
import sys


def check(cond, msg):
    if not cond:
        print("FAIL:", msg)
        sys.exit(1)


# --- 快形：类常量，名字从不出现在实例属性中 ---
class Fast:
    LIMIT = 1000

    def __init__(self):
        self.x = 1  # 共享键只含 x


def read_limit(o):
    return o.LIMIT


objs = [Fast() for _ in range(4)]
for _ in range(300):
    for o in objs:
        check(read_limit(o) == 1000, "fast-form classvar")

# 共享键成长（给实例添加同名属性）→ dk 版本前移 → 快形失效回 helper
shadow = Fast()
shadow.LIMIT = 7  # 名字进入共享键，遮蔽
check(read_limit(shadow) == 7, "late shadow via shared-keys growth")
check(read_limit(objs[0]) == 1000, "non-shadowed unchanged")

# --- 探测形：名字在共享键内，部分实例遮蔽 ---
class Probe:
    mode = "default"

    def __init__(self, override=None):
        if override is not None:
            self.mode = override  # 名字进共享键


def read_mode(o):
    return o.mode


mixed = [Probe(), Probe("special"), Probe(), Probe("alt")]
for _ in range(300):
    vals = [read_mode(o) for o in mixed]
    check(
        vals == ["default", "special", "default", "alt"],
        "probe-form mixed shadow %r" % (vals,))

# 后置遮蔽与删除
p = Probe()
for _ in range(50):
    read_mode(p)
p.mode = "late"
check(read_mode(p) == "late", "late instance shadow")
del p.mode
check(read_mode(p) == "default", "shadow deletion falls back to classvar")

# 类级改写 → tp_version_tag 失效
Probe.mode = "patched"
check(read_mode(Probe()) == "patched", "class-level reassignment")

# --- 描述符形态留 helper：property（数据）与方法（非数据）---
class WithDescr:
    def __init__(self, n):
        self._n = n

    @property
    def doubled(self):
        return self._n * 2

    def method(self):
        return self._n


def read_descr(o):
    return o.doubled, o.method()


for i in range(200):
    o = WithDescr(i)
    d, m = read_descr(o)
    check(d == i * 2 and m == i, "property/method via helper iter %d" % i)

# 绑定方法每次新建（is 语义不被缓存破坏）
w = WithDescr(1)
check(w.method is not w.method, "bound method freshness")

# --- kind-6：__slots__ 成员描述符（对象体直读内联）---
class Slotted:
    __slots__ = ("a", "b")

    def __init__(self, a=None):
        if a is not None:
            self.a = a


def read_slot(o):
    return o.a


sobjs = [Slotted(i) for i in range(4)]
for _ in range(300):
    for i, o in enumerate(sobjs):
        check(read_slot(o) == i, "slots read")

# 空槽必须抛 AttributeError（helper/PyMember_GetOne 语义）
empty = Slotted()
for _ in range(50):
    try:
        read_slot(empty)
        check(False, "expected AttributeError on empty slot")
    except AttributeError:
        pass

# 覆写与删除
s = Slotted(1)
s.a = 42
check(read_slot(s) == 42, "slot rewrite")
del s.a
try:
    read_slot(s)
    check(False, "expected AttributeError after del")
except AttributeError:
    pass

# --- 多态站点 ---
class A2:
    tag = "A"

    def __init__(self):
        self.pad = 1


class B2:
    tag = "B"

    def __init__(self):
        self.pad = 2


def read_tag(o):
    return o.tag


pair = [A2(), B2()]
for _ in range(300):
    check([read_tag(o) for o in pair] == ["A", "B"], "polymorphic kind-7")

print("ALL PASS")
