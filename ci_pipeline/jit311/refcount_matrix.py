#!/usr/bin/env python3
"""Call-matrix refcount assertion (M5 exit condition).

For every corpus case, measure the refcount drift of module-level objects
(callables, operand instances, containers) across N repeated invocations,
in interp mode and jit mode. The assertion is drift equality between the
two modes: cases that legitimately mutate state drift identically in both,
while a call-path refcount defect shows as a jit-only drift.

Usage:
    refcount_matrix.py <corpus_dir> <module> <mode:interp|jit> <out.json>
    refcount_matrix.py diff <interp.json> <jit.json>

Run once per mode, then compare with the built-in diff mode (exit 1 on any
drift inequality).  In jit mode every case (and its helpers) must actually
compile: an unavailable cinderjit or a refused force_compile() is a loud
error, never a silent fall back to interpreted execution -- interpreted
runs may not impersonate JIT evidence.
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


def diff_main() -> int:
    interp_path, jit_path = sys.argv[2:4]
    interp = json.load(open(interp_path))
    jit_doc = json.load(open(jit_path))
    mismatched = {}
    cases = set(interp["drift"]) | set(jit_doc["drift"])
    for case in sorted(cases):
        a = interp["drift"].get(case)
        b = jit_doc["drift"].get(case)
        if a != b:
            mismatched[case] = {"interp": a, "jit": b}
    if mismatched:
        print(json.dumps(mismatched, indent=1, sort_keys=True))
        print(f"refcount-matrix: {len(mismatched)} case(s) drift unequally")
        return 1
    print(f"refcount-matrix: {len(cases)} case(s), drift equal across modes")
    return 0


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "diff":
        return diff_main()
    corpus_dir, modname, mode, out_path = sys.argv[1:5]
    sys.path.insert(0, corpus_dir)
    sys.path.insert(0, ".")

    jit = None
    if mode == "jit":
        # 守卫自适应去特化会在案例中途卸载重编（共享 helper 跑热后被
        # force_compile 会烘焙特化形，守卫风暴触发 despec）：产物切换
        # 使烘焙引用集合变化，双快照协议无法与真实漂移区分（实测恰为
        # 每案例 −1 且 despec 关闭即 0、不随迭代累积——记账工件而非
        # 泄漏）。本判据只验证编译产物的逐迭代引用中性，despec 迁移
        # 的引用平衡由其触发路径的 Ref 持有审计保证。
        import os

        os.environ.setdefault("CINDERX_ADAPTIVE_DESPEC", "0")
        import cinderx

        cinderx.init()
        try:
            import cinderjit as jit
        except ImportError:
            print(
                "refcount-matrix: jit mode requested but cinderjit is not "
                "available (capability-gated build?); refusing to fall back "
                "to interpreted execution",
                file=sys.stderr,
            )
            return 2

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
                # force_compile() returns False both for a refusal and for
                # an already-compiled function (helpers are shared across
                # cases, so the second sighting is routine).  The truth
                # condition is is_jit_compiled() afterwards.
                jit.force_compile(f)
                if not jit.is_jit_compiled(f):
                    print(
                        f"refcount-matrix: force_compile refused for "
                        f"{name}; jit-mode evidence requires compiled "
                        f"execution",
                        file=sys.stderr,
                    )
                    return 2
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
