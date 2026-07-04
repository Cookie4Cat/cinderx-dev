#!/usr/bin/env python3
"""Call-matrix refcount assertion (M5 exit condition).

For every corpus case, measure the refcount drift of module-level objects
(callables, operand instances, containers) across N repeated invocations,
in interp mode and jit mode. The assertion is drift equality between the
two modes: cases that legitimately mutate state drift identically in both,
while a call-path refcount defect shows as a jit-only drift.

Usage:
    refcount_matrix.py <corpus_dir> <module> <mode:interp|jit> <out.json>

Run once per mode; compare with refcount_matrix_diff.py or a JSON diff.
Pseudo-immortal singletons (True/False/None/...) are excluded: their
refcounts drift by design on 3.11 (see M4-log).
"""

import gc
import importlib
import json
import sys
import types

N = 200

_rt = types.ModuleType("diffgate_rt")
_rt.checkpoint = lambda: None
sys.modules["diffgate_rt"] = _rt


def main() -> int:
    corpus_dir, modname, mode, out_path = sys.argv[1:5]
    sys.path.insert(0, corpus_dir)
    sys.path.insert(0, ".")

    jit = None
    if mode == "jit":
        import cinderx

        cinderx.init()
        import cinderjit as jit

    mod = importlib.import_module(f"corpus.{modname}")

    singletons = {id(True), id(False), id(None), id(NotImplemented), id(...)}
    targets = {}
    for name, obj in sorted(vars(mod).items()):
        if name.startswith("__") or isinstance(obj, types.ModuleType):
            continue
        if id(obj) in singletons:
            continue
        targets[name] = obj

    cases = [
        (n, f)
        for n, f in sorted(vars(mod).items())
        if callable(f) and n.startswith("case_")
    ]

    def snapshot():
        # 类型方法缓存（MCACHE）在 3.11 对缓存名字持强引用，条目被
        # 碰撞驱逐时合法释放——任何曾作为查找名的字符串目标都会因此
        # 产生与被测代码无关的 ±1 漂移（闪烁案 case_sub_index_protocol/
        # _pname：函数内建类使 tp_version 流水与名字哈希在窗口内随机
        # 撞槽驱逐旧名；jit 模式因拉式验证/IC 填充的额外类型查找改变
        # 缓存流量而更易触发）。快照前清空缓存，两侧均无 MCACHE 持
        # 引用，判据确定化。
        sys._clear_type_cache()
        return {n: sys.getrefcount(o) for n, o in targets.items()}

    results = {}
    for name, fn in cases:
        fns = [fn] + list(getattr(fn, "helpers", ()))
        if jit is not None:
            for f in fns:
                try:
                    jit.force_compile(f)
                except Exception:
                    pass  # refusal falls back to the interpreter
        # Warm up once: first-call effects (caches, quickening) are not part
        # of the steady-state drift contract.
        try:
            fn()
        except BaseException:
            pass
        gc.collect()
        before = snapshot()
        for _ in range(N):
            try:
                fn()
            except BaseException:
                pass
        gc.collect()
        after = snapshot()
        drift = {
            n: after[n] - before[n] for n in before if after[n] != before[n]
        }
        results[name] = drift

    json.dump(
        {"module": modname, "mode": mode, "iterations": N, "drift": results},
        open(out_path, "w"),
        indent=1,
        sort_keys=True,
    )
    drifting = {k: v for k, v in results.items() if v}
    print(f"{modname} [{mode}]: {len(cases)} cases, {len(drifting)} with drift")
    return 0


if __name__ == "__main__":
    sys.exit(main())
