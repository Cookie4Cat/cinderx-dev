"""store 桩 values 形行内插入的语义冒烟。

要点：插入序字节（values 预头 [-2-size]）驱动实例字典物化后的键序，
桩的行内插入若写错序号或尺寸字节，vars() 键序与 stock 分歧即现形。
覆盖：乱序插入的键序、覆写/删除后重插、自赋值、超容量回落、混合
读写正确性。store 位点须编译（函数体内写属性，调用次数越过阈值）。
"""

import sys


class P:
    pass


def warm_shape(o, a, b, c, d):
    o.a = a
    o.b = b
    o.c = c
    o.d = d


def set_cba(o):
    o.c = 30
    o.b = 20
    o.a = 10


def set_a_only(o):
    o.a = 99


def overwrite_then_read(o):
    o.a = 1
    o.a = o.a
    o.a = 2
    return o.a


def main():
    # 预热：固化共享键序 a,b,c,d 并使各 store 位点编译。
    for i in range(64):
        warm_shape(P(), i, i, i, i)
        set_cba(P())
        set_a_only(P())

    # ① 乱序插入：共享键序 a,b,c,d，实例按 c,b,a 插入——物化键序
    #    必须是插入序而非共享键序。
    o = P()
    set_cba(o)
    keys = list(vars(o).keys())
    assert keys == ["c", "b", "a"], keys
    assert (o.a, o.b, o.c) == (10, 20, 30)

    # ② 单键插入后物化。
    o = P()
    set_a_only(o)
    assert list(vars(o).keys()) == ["a"]
    assert o.a == 99

    # ③ 覆写与自赋值（旧路径回归）。
    o = P()
    assert overwrite_then_read(o) == 2
    assert list(vars(o).keys()) == ["a"]

    # ④ 删除后重插：删除应从插入序移除，重插回到尾部。
    o = P()
    set_cba(o)
    del o.b
    for _ in range(4):
        o.b = 7  # 首次为插入，其后为覆写
    assert list(vars(o).keys()) == ["c", "a", "b"], list(vars(o).keys())
    assert o.b == 7

    # ⑤ 物化实例上的插入（-3 槽字典）走回落路径仍正确。
    o = P()
    set_cba(o)
    vars(o)  # 物化
    o.d = 40
    assert list(vars(o).keys()) == ["c", "b", "a", "d"]
    assert o.d == 40

    # ⑥ 混合负载一致性：与纯字典构建对照。
    for i in range(64):
        o = P()
        set_cba(o)
        assert vars(o) == {"c": 30, "b": 20, "a": 10}, vars(o)

    # ⑦ kind-7 遮蔽写（非数据描述符位点的实例字典写）。
    class Memo:
        def __get__(self, obj, owner=None):
            if obj is None:
                return self
            val = obj.base * 10
            obj.cached = val  # 遮蔽写位点（kind-7 store）
            return val

    class Q:
        cached = Memo()

        def __init__(self, base):
            self.base = base

    def touch(q):
        q.cached = q.cached + 1  # 读（描述符/遮蔽）+ 遮蔽覆写
        return q.cached

    # 预热：位点编译 + 条目定型。
    for i in range(64):
        q = Q(i)
        touch(q)
    # values 形遮蔽写：首写为插入、再写为覆写；描述符本体不受扰动。
    q = Q(7)
    assert touch(q) == 71
    assert q.cached == 71
    assert touch(q) == 72
    assert type(Q.__dict__["cached"]) is Memo
    # 删除遮蔽后描述符复现，再写重建遮蔽。
    del q.cached
    assert q.cached == 70  # __get__ 重新缓存
    q.cached = 5
    assert q.cached == 5
    # 物化后的遮蔽覆写。
    vars(q)
    q.cached = 9
    assert q.cached == 9
    assert vars(q)["cached"] == 9

    print("OK smoke_store_insert")
    return 0


sys.exit(main())
