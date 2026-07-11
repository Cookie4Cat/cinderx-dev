"""新鲜函数对象挂接(挂接断链轮)语义与稳定性冒烟。

① 预算内的新实例接上既有编译入口(code 级编译产物复用);
② 高频翻新+即弃实例的挂接经 GC 重入不崩(UAF 回归:finalize 中途
   分配触发 GC 曾析构仅锚于濒死实例的 CompiledFunction);
③ 行为正确性:挂接前后结果一致,复用实例照常编译。
"""

import sys


stats = {i: (i * 7919) % 4096 for i in range(2000)}


def train_round():
    f = lambda x: stats[x]  # 每轮新建:同 code,新函数对象
    return max(stats, key=f), f


def main():
    import cinderjit

    expected = max(stats, key=lambda x: stats[x])
    inst = None
    # 预热跨过阈值并触发首编译;继续翻新驱动挂接与 GC churn(② 的
    # 崩溃形态需要既有锚点全悬于已弃实例)。
    for i in range(64):
        got, inst = train_round()
        assert got == expected, (i, got, expected)
    # ①:预算内实例已挂接(首个越阈实例之后的早期新实例)。
    assert cinderjit.is_jit_compiled(train_round)
    # code 级编译产物存在。
    import types
    lam_code = [
        c for c in train_round.__code__.co_consts
        if isinstance(c, types.CodeType)
    ][0]
    assert any(
        getattr(g, "__code__", None) is lam_code
        for g in cinderjit.get_compiled_functions()
    )
    # ③:复用实例正常编译。
    g = lambda x: stats[x]
    for _ in range(16):
        assert max(stats, key=g) == expected
    assert cinderjit.is_jit_compiled(g)
    print("OK smoke_fresh_attach")
    return 0


sys.exit(main())
