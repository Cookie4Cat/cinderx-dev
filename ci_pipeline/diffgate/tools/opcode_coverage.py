#!/usr/bin/env python3
"""Opcode coverage of the diffgate corpus vs the running Python.

Answers "is the corpus comprehensive enough" with data instead of opinion.
Executes each corpus module (with a stub diffgate_rt, cinderx never loaded),
walks every function object in the module namespace — including call sites
generated via exec at import time — plus the module-level code itself, and
reports which opcodes of this interpreter are never emitted.

Run with the *target* Python version; numbers are per-version.

Usage: python3.11 tools/opcode_coverage.py [--corpus DIR] [--fail-under N]
"""

import argparse
import dis
import opcode
import pathlib
import sys
import types

# Opcodes that legitimately never appear in corpus bytecode.
NOT_APPLICABLE = {
    "CACHE",       # inline-cache placeholder, runtime artifact
    "PRINT_EXPR",  # interactive mode only
}
# Deliberately out of scope with a recorded reason (design decisions).
DEFERRED = {
    "GET_AITER": "async: 拒编范围（D6），M8 补拒编断言",
    "GET_ANEXT": "async: 拒编范围（D6）",
    "GET_AWAITABLE": "async: await 语义，D6",
    "BEFORE_ASYNC_WITH": "async with，D6",
    "END_ASYNC_FOR": "async for，D6",
    "ASYNC_GEN_WRAP": "async generator，D6",
    "IMPORT_NAME": "import 域，M4 随前端适配补",
    "IMPORT_FROM": "import 域，M4",
    "IMPORT_STAR": "import 域，M4",
    "SETUP_ANNOTATIONS": "模块/类体注解，M4",
}


def walk_code(code, seen):
    for ins in dis.get_instructions(code):
        seen.add(ins.opname)
    for const in code.co_consts:
        if hasattr(const, "co_code"):
            walk_code(const, seen)


def collect_module(path, seen):
    source = path.read_text(encoding="utf-8")
    module_code = compile(source, str(path), "exec")
    walk_code(module_code, seen)

    # Execute to make exec-generated functions visible.
    rt_stub = types.ModuleType("diffgate_rt")
    rt_stub.mode = "coverage"
    rt_stub.checkpoint = lambda: None
    sys.modules.setdefault("diffgate_rt", rt_stub)
    namespace = {"__name__": path.stem, "__file__": str(path)}
    exec(module_code, namespace)

    visited = set()

    def visit(value):
        fn = getattr(value, "__func__", value)
        code = getattr(fn, "__code__", None)
        if code is not None and id(code) not in visited:
            visited.add(id(code))
            walk_code(code, seen)
        # case 函数挂载的 helpers（exec 生成的调用点/运算函数常在这里）
        for helper in getattr(fn, "helpers", ()):
            visit(helper)
        # 类体方法
        if isinstance(value, type):
            for member in vars(value).values():
                visit(member)
        # 模块级 dict 容器（如 SHAPE_FNS）走一层
        if isinstance(value, dict):
            for member in value.values():
                if callable(member):
                    visit(member)

    for value in namespace.values():
        visit(value)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--corpus",
        default=str(pathlib.Path(__file__).resolve().parent.parent / "corpus"))
    ap.add_argument("--fail-under", type=int, default=0,
                    help="exit 1 if covered opcode count is below N")
    args = ap.parse_args()

    seen = set()
    for path in sorted(pathlib.Path(args.corpus).glob("corpus_*.py")):
        collect_module(path, seen)

    all_ops = set(opcode.opmap)
    relevant = all_ops - NOT_APPLICABLE - set(DEFERRED)
    covered = seen & relevant
    missing = sorted(relevant - seen)

    print(f"python: {sys.version.split()[0]}")
    print(f"opcodes: {len(all_ops)} total, {len(relevant)} in scope, "
          f"{len(covered)} covered, {len(missing)} missing")
    if missing:
        print("missing (in scope):")
        for i in range(0, len(missing), 4):
            print("  " + "  ".join(missing[i:i + 4]))
    print(f"deferred with reasons: {len(DEFERRED)}（async/import/annotations，"
          f"见本文件 DEFERRED 表)")

    if args.fail_under and len(covered) < args.fail_under:
        print(f"FAIL: covered {len(covered)} < required {args.fail_under}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
