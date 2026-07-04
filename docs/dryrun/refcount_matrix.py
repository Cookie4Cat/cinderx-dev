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
