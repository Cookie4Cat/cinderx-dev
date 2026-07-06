# M10 全量摸底三家族修复的冒烟集（各为当轮最小复现的固化）。
# 运行：PYTHONPATH=<sitecustomize:cinderx> PYTHONJITAUTO=2 python3.11 smoke_m10_fixes.py
# 预期输出四行 PASS 与末行 ALL PASS；任何崩溃/断言失败即回归。

import enum


def check_ic_deleted_entry():
    # 家族一：物化实例字典含已删除条目（me_key=NULL）时，
    # kDescrOrClassVar 提示读的线性扫描必须跳过（getDictKeysIndex）。
    class A:
        logger = "A-logger"

    class B:
        logger = "B-logger"

    def read(o):
        t = 0
        for _ in range(50):
            t += len(o.logger)
        return t

    a, b = A(), B()
    for i in range(30):
        setattr(a, "attr%d" % i, i)  # 撑爆共享键 -> 自然物化为 combined
    delattr(a, "attr0")  # 留下 me_key=NULL 已删除条目
    for _ in range(5):
        read(a)
        read(b)  # 多态站点，强制运行时 IC 路径
    assert read(a) == 50 * len("A-logger")
    print("PASS ic_deleted_entry")


def check_raw_null_pair_kw_call():
    # 家族二：模块函数 + 关键字调用（3.11 编译为 LOAD_GLOBAL 带 NULL
    # 标志 + LOAD_ATTR，原始 NULL 在 callable 槽）。simplify 预算耗尽
    # 时原始形态漏到运行时，JITRT_Call 必须按 3.11 原始约定移位。
    import re

    def f():
        t = 0
        for _ in range(30):
            s = re.sub("a", "", "banana", count=0)
            t += len(s)
        return t

    for _ in range(5):
        f()
    assert f() == 30 * len("bnn")
    print("PASS raw_null_pair_kw_call")


def check_normalized_pair_classmethod():
    # 家族二反面：LoadMethodResult 归一化形态 {callable, NULL}——
    # 类方法经实例方法缓存慢路径产出，JITRT_Call 须跳过 NULL 第二槽
    # 而非把 callable 误判。Enum 动态建类每次触发
    # EnumType._check_for_existing_members_（classmethod）调用。
    for i in range(20):
        enum.Enum("Color%d" % i, ["A", "B", "C"])
    print("PASS normalized_pair_classmethod")


def check_code_swap_invalidation():
    # 家族三：__code__ 运行期替换后编译入口必须失效（3.11 无 function
    # watcher，入口做 func_code 身份拉式校验）。networkx argmap 的
    # 惰性编译即"首调后自替换 __code__"模式。
    def old():
        return "old"

    def new():
        return "new"

    for _ in range(6):
        old()  # 触发编译

    old.__code__ = new.__code__
    assert old() == "new", "stale compiled entry after __code__ swap"
    print("PASS code_swap_invalidation")


check_ic_deleted_entry()
check_raw_null_pair_kw_call()
check_normalized_pair_classmethod()
check_code_swap_invalidation()
print("ALL PASS")
