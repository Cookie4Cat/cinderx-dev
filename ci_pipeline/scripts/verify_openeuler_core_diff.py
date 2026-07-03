#!/usr/bin/env python3
"""openEuler python3 SRPM vs upstream CPython: JIT core file set diff.

Design anchor: adaptation-plan D7 and M1 exit condition #5. The version
anchor is the openEuler 24.03-LTS python3 package (3.11.6 + distro patches).
This script machine-verifies the vendor-source decision:

    * core set diff empty  -> vendor the JIT-relevant files from upstream
                              v3.11.6 (distro patches don't touch them);
    * core set diff nonzero -> vendor from the openEuler patched tree and
                              record the differing files as review targets.

Inputs are two *prepared* source trees:

    --upstream-tree   e.g. `git archive v3.11.6` of cpython, extracted;
    --openeuler-tree  the SRPM after `%prep`, i.e.:
                          rpm -i python3-3.11.6-N.oeXXXX.src.rpm
                          rpmbuild -bp --nodeps ~/rpmbuild/SPECS/python3.spec
                          -> ~/rpmbuild/BUILD/Python-3.11.6

The core set below is the "JIT-relevant" surface: the interpreter loop and
everything CinderX vendors, borrows or replaces semantics of (frames, code
objects, calls, generators, dicts/types for the IC layer, opcode metadata).
stdlib CVE errata deliberately stay outside so the gate does not cry wolf.
"""

from __future__ import annotations

import argparse
import difflib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

CORE_FILE_SET: tuple[str, ...] = (
    # Interpreter loop + eval breaker (M2 vendor target)
    "Python/ceval.c",
    "Python/ceval_gil.h",
    # Frames (M3), thread/interp state incl. _PyThreadState_PopFrame (borrow)
    "Python/frame.c",
    "Python/pystate.c",
    "Objects/frameobject.c",
    # PEP 659 specialization: caches CinderX reads through _PyOpcode_Deopt
    "Python/specialize.c",
    # Code objects / functions / calls (M4/M5)
    "Objects/codeobject.c",
    "Objects/funcobject.c",
    "Objects/call.c",
    "Objects/genobject.c",
    # IC layer version-tag semantics (M7, D5)
    "Objects/dictobject.c",
    "Objects/typeobject.c",
    # Internal headers the JIT compiles against
    "Include/internal/pycore_frame.h",
    "Include/internal/pycore_code.h",
    "Include/internal/pycore_ceval.h",
    "Include/internal/pycore_pystate.h",
    "Include/internal/pycore_opcode.h",
    # Opcode surface (M1 opcode table verifier ties to this)
    "Include/opcode.h",
    "Lib/opcode.py",
)


def compare_file(upstream: Path, openeuler: Path) -> dict:
    if not upstream.exists():
        return {"status": "missing_upstream"}
    if not openeuler.exists():
        return {"status": "missing_openeuler"}
    a = upstream.read_bytes()
    b = openeuler.read_bytes()
    if a == b:
        return {"status": "identical"}
    a_lines = a.decode("utf-8", errors="replace").splitlines(keepends=True)
    b_lines = b.decode("utf-8", errors="replace").splitlines(keepends=True)
    diff = list(
        difflib.unified_diff(a_lines, b_lines, "upstream", "openeuler", n=2)
    )
    changed = sum(1 for line in diff if line[:1] in "+-" and line[:3] not in ("+++", "---"))
    return {
        "status": "differs",
        "changed_lines": changed,
        "diff_head": "".join(diff[:80]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-tree", type=Path, required=True)
    parser.add_argument("--openeuler-tree", type=Path, required=True)
    parser.add_argument(
        "--report",
        type=Path,
        default=Path("openeuler-core-diff-report.json"),
    )
    parser.add_argument(
        "--srpm-id",
        default="",
        help="Provenance note, e.g. python3-3.11.6-31.oe2403sp3.src.rpm",
    )
    args = parser.parse_args()

    results: dict[str, dict] = {}
    for rel in CORE_FILE_SET:
        results[rel] = compare_file(
            args.upstream_tree / rel, args.openeuler_tree / rel
        )

    differing = [r for r, v in results.items() if v["status"] == "differs"]
    missing = [r for r, v in results.items() if v["status"].startswith("missing")]
    verdict = (
        "vendor-from-upstream"
        if not differing and not missing
        else "vendor-from-openeuler-tree"
    )

    report = {
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "srpm": args.srpm_id,
        "upstream_tree": str(args.upstream_tree),
        "openeuler_tree": str(args.openeuler_tree),
        "core_file_count": len(CORE_FILE_SET),
        "verdict": verdict,
        "differing": differing,
        "missing": missing,
        "files": results,
    }
    args.report.write_text(json.dumps(report, indent=2) + "\n")

    print(f"core file set: {len(CORE_FILE_SET)} files")
    for rel in CORE_FILE_SET:
        entry = results[rel]
        line = f"  {entry['status']:18s} {rel}"
        if entry["status"] == "differs":
            line += f"  ({entry['changed_lines']} changed lines)"
        print(line)
    print(f"verdict: {verdict}")
    print(f"report: {args.report}")

    # Missing files mean the trees are not prepared correctly -- that is an
    # error in the pipeline, not a legitimate "differs" outcome.
    return 2 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
