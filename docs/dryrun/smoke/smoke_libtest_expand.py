"""libtest 扩充轮回归冒烟。

两个形态均为关机窗口崩溃，子进程退出码判定，各跑 3 次：
① _PyGen_yf 兜底新引用约定——await 一只已在 await 中的协程后正常退出
   （借用形返回曾致 GET_AWAITABLE 配对 Py_DECREF 过度递减，挂起协程栈上
   的 awaitee 成悬垂指针，关机周期 GC 段错）；
② co_extra 最小容量写入——第三方注册 Python 级 freefunc 后，本运行时
   碰过的 code 不得把 extras 数组扩到第三方索引（vanilla code_dealloc
   对 ce_size 覆盖的索引无条件调 freefunc，关机晚期将回调 Python 而
   段错）。
"""

import os
import subprocess
import sys
import tempfile

CASES = {
    "genyf_await15": """
import types
@types.coroutine
def nop(): yield
async def c(): await nop()
async def waiter(co): await co
coro = c(); coro.send(None)
try:
    waiter(coro).send(None)
except RuntimeError as e:
    assert "being awaited" in str(e), e
""",
    "coextra_min_alloc": """
import ctypes
py = ctypes.pythonapi
FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_voidp)
Req = py._PyEval_RequestCodeExtraIndex
Req.argtypes = (FREEFUNC,); Req.restype = ctypes.c_ssize_t
SetExtra = py._PyCode_SetExtra
SetExtra.argtypes = (ctypes.py_object, ctypes.c_ssize_t, ctypes.c_voidp)
SetExtra.restype = ctypes.c_int
def myfree(ptr): pass
FREE_FUNC = FREEFUNC(myfree)
IDX = Req(FREE_FUNC)
f = eval("lambda: 42")
SetExtra(f.__code__, IDX, ctypes.c_voidp(100))
del f
def warm(x): return x + 1
for i in range(64): warm(i)
""",
}


def main():
    failed = []
    for name, body in CASES.items():
        with tempfile.NamedTemporaryFile(
            "w", suffix=".py", delete=False
        ) as tmp:
            tmp.write(body)
            path = tmp.name
        try:
            for round_no in range(3):
                proc = subprocess.run(
                    [sys.executable, path], capture_output=True, timeout=120
                )
                if proc.returncode != 0:
                    print(
                        "FAIL {} round={} rc={}".format(
                            name, round_no, proc.returncode
                        )
                    )
                    sys.stderr.buffer.write(proc.stderr[-400:])
                    failed.append(name)
                    break
            else:
                print("OK {}".format(name))
        finally:
            os.unlink(path)
    sys.exit(1 if failed else 0)


main()
