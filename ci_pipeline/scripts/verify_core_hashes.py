#!/usr/bin/env python3
"""Source-hash gate for the CPython 3.11 vendored/generated core file set.

Design anchor: adaptation-plan D3 ("vendored ceval + patch files + build-time
source hash check") and M1 exit condition #4 ("hash mismatch fails the
build"). The gate locks the 3.11 vendored surface so that silent edits are
impossible: any change must come with a manifest re-bless, which is what code
review hooks onto.

Scope note: only the *vendored/generated* 3.11 core set is locked. Shared
C++ (Jit/, Common/, ...) is intentionally out of scope -- it is guarded by
the dual-version compile gate, not by hashes.

Usage:
    verify_core_hashes.py --verify            # default; exit 1 on mismatch
    verify_core_hashes.py --generate          # (re-)bless the current tree
    verify_core_hashes.py --verify --json OUT # also write machine report

Build wiring (formal M1): call with --verify from setup.py BuildExt.run()
and/or a CMake custom target that _cinderx depends on, so a mismatch fails
the build before compilation starts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "ci_pipeline" / "core_hashes_311.json"

# The locked 3.11 vendored/generated surface. Glob patterns are relative to
# the repository root. Extend as M2 vendors ceval.c and its patch files.
LOCK_PATTERNS: tuple[str, ...] = (
    "cinderx/Interpreter/3.11/**/*",
    "cinderx/PythonLib/opcodes/3.11/opcode.py",
    "cinderx/UpstreamBorrow/borrowed-3.11-fallback.c",
)


def iter_locked_files(root: Path) -> list[Path]:
    files: set[Path] = set()
    for pattern in LOCK_PATTERNS:
        for path in root.glob(pattern):
            if path.is_file():
                files.add(path)
    return sorted(files)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def generate(root: Path, manifest_path: Path) -> int:
    files = iter_locked_files(root)
    if not files:
        print(f"error: no files matched lock patterns under {root}")
        return 1
    manifest = {
        "comment": (
            "Locked hashes for the CPython 3.11 vendored/generated core "
            "set. Regenerate with verify_core_hashes.py --generate; the "
            "diff of this file is what reviewers approve."
        ),
        "generated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "patterns": list(LOCK_PATTERNS),
        "files": {
            str(path.relative_to(root)): sha256_of(path) for path in files
        },
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"blessed {len(files)} files -> {manifest_path}")
    return 0


def verify(root: Path, manifest_path: Path, json_out: Path | None) -> int:
    if not manifest_path.exists():
        print(f"error: manifest {manifest_path} missing; run --generate first")
        return 1
    manifest = json.loads(manifest_path.read_text())
    expected: dict[str, str] = manifest["files"]

    actual = {
        str(path.relative_to(root)): sha256_of(path)
        for path in iter_locked_files(root)
    }

    mismatched = sorted(
        rel for rel in expected.keys() & actual.keys()
        if expected[rel] != actual[rel]
    )
    missing = sorted(expected.keys() - actual.keys())
    unlocked = sorted(actual.keys() - expected.keys())

    report = {
        "manifest": str(manifest_path),
        "checked": len(actual),
        "mismatched": mismatched,
        "missing": missing,
        "unlocked_new_files": unlocked,
        "ok": not (mismatched or missing or unlocked),
    }
    if json_out is not None:
        json_out.write_text(json.dumps(report, indent=2) + "\n")

    for rel in mismatched:
        print(f"HASH MISMATCH: {rel}")
    for rel in missing:
        print(f"MISSING (locked but absent): {rel}")
    for rel in unlocked:
        print(f"UNLOCKED (new file in locked area, needs bless): {rel}")

    if report["ok"]:
        print(f"core hash gate OK ({len(actual)} files)")
        return 0
    print(
        "core hash gate FAILED. If the change is intentional, regenerate "
        "the manifest with --generate and include it in the review."
    )
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", default=True)
    mode.add_argument("--generate", action="store_true")
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args()

    if args.generate:
        return generate(args.root, args.manifest)
    return verify(args.root, args.manifest, args.json)


if __name__ == "__main__":
    sys.exit(main())
