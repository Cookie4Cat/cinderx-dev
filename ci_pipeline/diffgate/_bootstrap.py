#!/usr/bin/env python3
"""diffgate child runner: execute one corpus module under one mode.

Runs inside the *target* Python (bare metal or container).
Modes:
  interp    - plain interpreter, cinderx never imported (the oracle).
  jit       - cinderx JIT, every case fn (and declared helpers) force-compiled.
  jit_deopt - like jit, but diffgate_rt.checkpoint() force-uncompiles the
              current case's functions mid-execution (entrypoint swap; frames
              already on stack / suspended generators exercise the deopt path
              on their next resume).

Output protocol (stdout), one line per case:
  CASE <name> OK <repr(value)>
  CASE <name> EXC <ExcType>: <message> @<func>+<relative-lineno>
Lines not starting with CASE/META are ignored by the orchestrator.
"""

import importlib
import sys
import traceback
import types


def _collect_cases(mod):
    cases = []
    for name in sorted(dir(mod)):
        if name.startswith("case_"):
            fn = getattr(mod, name)
            if callable(fn):
                cases.append((name, fn))
    return cases


def _exc_line(name, exc):
    frames = list(traceback.walk_tb(exc.__traceback__))
    if frames:
        frame, lineno = frames[-1]
        code = frame.f_code
        loc = "@{}+{}".format(code.co_name, lineno - code.co_firstlineno)
    else:
        loc = "@?"
    return "CASE {} EXC {}: {} {}".format(name, type(exc).__name__, exc, loc)


def main():
    mode, corpus_dir, modname = sys.argv[1], sys.argv[2], sys.argv[3]
    skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    sys.path.insert(0, corpus_dir)

    # Runtime shim importable by corpus modules; checkpoint is a no-op except
    # in jit_deopt mode where the per-case closure below replaces it.
    rt = types.ModuleType("diffgate_rt")
    rt.mode = mode
    rt.checkpoint = lambda: None
    sys.modules["diffgate_rt"] = rt

    jit = None
    if mode in ("jit", "jit_deopt"):
        import cinderx

        cinderx.init()
        import cinderjit as jit

        if not jit.is_enabled():
            print("META error=jit_not_enabled")
            return 2

    mod = importlib.import_module(modname)
    cases = _collect_cases(mod)[skip:]
    print("META mode={} module={} cases={}".format(mode, modname, len(cases)))
    sys.stdout.flush()

    for name, fn in cases:
        fns = [fn] + list(getattr(fn, "helpers", ()))
        if jit is not None:
            # 编译前先解释执行一遍完成 quickening（特化缓存就位），
            # 镜像 auto 阈值"热身后编译"的真实时序：未量化编译走不到
            # WITH_VALUES 等特化快路径及其守卫，正是有机 deopt-resume
            # 家族（M9R3）长期漏网的覆盖缺口。语料纪律要求用例自恢复，
            # 重复执行输出不变；热身轮的异常同样完成量化，照常吞掉。
            try:
                fn()
            except BaseException:
                pass
            for f in fns:
                try:
                    jit.force_compile(f)
                except Exception:
                    pass  # rejected-to-compile is a legitimate outcome
            if mode == "jit_deopt":

                def _checkpoint(_fns=tuple(fns)):
                    for f in _fns:
                        try:
                            jit.force_uncompile(f)
                        except Exception:
                            pass

                rt.checkpoint = _checkpoint
        try:
            value = fn()
            print("CASE {} OK {!r}".format(name, value))
        except BaseException as exc:  # each case's outcome is the record
            print(_exc_line(name, exc))
        finally:
            sys.stdout.flush()  # survive a SEGV in the next case
            rt.checkpoint = lambda: None
    return 0


if __name__ == "__main__":
    sys.exit(main())
