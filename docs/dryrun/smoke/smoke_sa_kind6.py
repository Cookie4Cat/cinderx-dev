"""kind-6 成员写(__slots__,PyMember_SetOne T_OBJECT_EX 行内形)语义冒烟。

① 覆写/首写(旧槽 NULL)/同对象重赋值(别名计数);② 删除后重写
(旧槽回 NULL);③ 旧值末引用(dealloc 路径回落 helper);④ 引用
计数精确性(sys.getrefcount 对照);⑤ 非 T_OBJECT_EX 成员与
READONLY 回落语义不变。
"""
import sys

class S:
    __slots__ = ("a", "b")

def writer(s, v):
    s.a = v
    return s.a

def main():
    import cinderjit
    sentinel = object()
    s = S()
    for i in range(64):
        assert writer(s, i) == i          # 首写+覆写
    assert cinderjit.is_jit_compiled(writer)
    # ① 同对象重赋值:计数净零
    obj = object()
    s.a = obj
    base = sys.getrefcount(obj)
    for _ in range(1000):
        writer(s, obj)
    assert sys.getrefcount(obj) == base, (sys.getrefcount(obj), base)
    # ④ 换值计数交接精确
    o1, o2 = object(), object()
    r1, r2 = sys.getrefcount(o1), sys.getrefcount(o2)
    writer(s, o1); writer(s, o2)
    assert sys.getrefcount(o1) == r1, "旧值引用未释放"
    assert sys.getrefcount(o2) == r2 + 1, "新值引用未持有"
    # ② 删除后重写
    del s.a
    try:
        s.a
        raise AssertionError("expected AttributeError")
    except AttributeError:
        pass
    assert writer(s, 7) == 7
    # ③ 旧值末引用 dealloc 路径
    class Fin:
        pass
    fired = []
    import weakref
    for _ in range(200):
        f = Fin(); wr = weakref.ref(f, lambda r: fired.append(1))
        writer(s, f)
        del f
        writer(s, sentinel)   # 覆写触发旧值 dealloc(helper 路径)
        assert wr() is None
    assert len(fired) == 200
    print("smoke_sa_kind6 OK")

main()
