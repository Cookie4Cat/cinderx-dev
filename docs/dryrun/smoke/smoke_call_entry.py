"""调用位点入口缓存(被调方行内压栈轮)语义冒烟。

覆盖面按风险矩阵展开:
① 直达命中正确性:JIT 调用方 → JIT 被调方(精确实参、多参、缺省
   参数带满实参),含返回值与副作用一致性;
② 递归预检保留:深递归经缓存位点必须 RecursionError 而非段错误;
③ tracing 语义保留:sys.settrace 激活期间缓存命中整调用分流解释器,
   trace 事件不缺失;
④ __code__ 换体:运行期替换 __code__ 后经 code 恒等守卫落 miss,
   按新 code 语义执行(networkx argmap 惰性编译模式);
⑤ 参数仪式不可省形态走中性/慢径:错实参数(缺省补齐)、kwargs 调用
   (不发射探测)、varargs/kwonly 被调方(拒直达填充)语义不变;
⑥ 多态位点:同一位点交替 Python 函数/内建/不同函数,预算耗尽后
   行内短路,结果恒正确;
⑦ 新鲜函数对象(推导式/lambda 翻新)经 code+vectorcall 守卫命中,
   挂接前后结果一致;
⑧ 生成器/协程函数经缓存位点创建照常。
"""

import sys


def add3(a, b, c):
    return a + b + c


def with_default(a, b=10):
    return a * b


def kwonly_fn(a, *, k=5):
    return a - k


def varargs_fn(*args):
    return len(args)


def gen_fn(n):
    for i in range(n):
        yield i * i


def deep(n):
    if n <= 0:
        return 0
    return deep(n - 1) + 1


def call_add3(i):
    return add3(i, i + 1, i + 2)


def call_with_default_full(i):
    return with_default(i, 3)


def call_with_default_short(i):
    return with_default(i)


def call_kwargs(i):
    return with_default(i, b=4)


def call_kwonly(i):
    return kwonly_fn(i)


def call_varargs(i):
    return varargs_fn(i, i, i)


def call_gen(i):
    return sum(gen_fn(5))


def poly_site(f, i):
    return f(i)


def call_deep(n):
    return deep(n)


def fresh_round(i):
    f = lambda x: x * 2 + i  # 每轮新函数对象,同 code
    return f(i)


def main():
    import cinderjit

    # ①⑤⑦⑧ 预热:全位点跨阈值,调用方与被调方都进 JIT。
    for i in range(64):
        assert call_add3(i) == 3 * i + 3, i
        assert call_with_default_full(i) == 3 * i, i
        assert call_with_default_short(i) == 10 * i, i
        assert call_kwargs(i) == 4 * i, i
        assert call_kwonly(i) == i - 5, i
        assert call_varargs(i) == 3, i
        assert call_gen(i) == 30, i
        assert fresh_round(i) == 3 * i, i
    for f in (call_add3, add3, call_with_default_full, with_default,
              call_kwonly, call_varargs, call_gen):
        assert cinderjit.is_jit_compiled(f), f.__name__

    # ⑥ 多态位点:Python 函数 ↔ 内建交替远超 helper 预算(64)。
    import math
    inc = lambda x: x + 1
    for i in range(300):
        assert poly_site(abs, -i) == i
        assert poly_site(inc, i) == i + 1
        assert poly_site(math.floor, i + 0.5) == i
        assert poly_site(deep, 3) == 3

    # ② 深递归:必须 RecursionError,不得段错误。
    try:
        call_deep(10**6)
        raise AssertionError("deep recursion did not raise")
    except RecursionError:
        pass
    # 递归后浅调用照常。
    assert call_deep(10) == 10

    # ③ tracing:激活期缓存命中整调用分流,call 事件不缺失。
    events = []

    def tracer(frame, event, arg):
        if event == "call":
            events.append(frame.f_code.co_name)
        return None

    sys.settrace(tracer)
    got = call_add3(7)
    sys.settrace(None)
    assert got == 24, got
    assert "add3" in events, events
    # tracing 关闭后恢复直达,结果一致。
    assert call_add3(8) == 27

    # ④ __code__ 换体:同位点旧函数对象换 code,守卫必须失效。
    def donor(a, b, c):
        return a * b * c

    add3.__code__ = donor.__code__
    try:
        for i in range(1, 8):
            assert call_add3(i) == i * (i + 1) * (i + 2), i
    finally:
        pass

    print("smoke_call_entry OK")


if __name__ == "__main__":
    main()
