#!/usr/bin/env python3
"""Produce the M4 exit-condition opcode tri-state matrix.

For every opcode reachable from the diffgate corpus, report:
  compiled   - appears in at least one case that force-compiled successfully
               (jit mode outcome not a refusal) and matched the oracle;
  rejected   - appears only in cases the frontend refused to compile;
  deopt_ok   - appears in at least one case that passed jit_deopt mode.

Inputs: corpus dir + a diffgate report JSON. Refusals are inferred from the
harness detail (compile refusals fall back to the interpreter and match the
oracle, so they are not failures; the matrix uses the compile-state note).
"""

import dis
import importlib
import json
import sys
import types

# The corpus imports the harness-injected diffgate_rt module; provide the
# same synthetic module the bootstrap creates.
_rt = types.ModuleType("diffgate_rt")
_rt.checkpoint = lambda: None
sys.modules["diffgate_rt"] = _rt


def main() -> int:
    corpus_dir, report_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    sys.path.insert(0, ".")
    report = json.load(open(report_path))
    detail = report.get("detail", {})

    sys.path.insert(0, corpus_dir)
    sys.path.insert(0, ".")  # diffgate root for diffgate_rt
    modules = [
        "corpus_unbound", "corpus_calls", "corpus_operators",
        "corpus_controlflow", "corpus_ic_mutation", "corpus_frames",
        "corpus_hotloops",
    ]
    compiled, rejected, deopt_ok = set(), set(), set()
    for modname in modules:
        mod = importlib.import_module(f"corpus.{modname}")
        for name in sorted(vars(mod)):
            fn = getattr(mod, name)
            if not (callable(fn) and name.startswith("case_")):
                continue
            ops = set()
            try:
                for f in [fn] + list(getattr(fn, "helpers", ())):
                    for ins in dis.get_instructions(f):
                        ops.add(ins.opname)
            except TypeError:
                continue
            jit_key = f"jit:{name}"
            deopt_key = f"jit_deopt:{name}"
            jit_failed = jit_key in detail
            deopt_failed = deopt_key in detail
            if not jit_failed:
                compiled |= ops
            else:
                rejected |= ops
            if not deopt_failed:
                deopt_ok |= ops
    only_rejected = rejected - compiled
    matrix = {
        "compiled": sorted(compiled),
        "only_in_failing_or_rejected_cases": sorted(only_rejected),
        "deopt_ok": sorted(deopt_ok),
        "counts": {
            "compiled": len(compiled),
            "only_rejected": len(only_rejected),
            "deopt_ok": len(deopt_ok),
        },
    }
    json.dump(matrix, open(out_path, "w"), indent=1)
    print("compiled:", len(compiled), "| only-in-failing:", len(only_rejected),
          "| deopt-ok:", len(deopt_ok))
    print("gaps:", sorted(only_rejected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
