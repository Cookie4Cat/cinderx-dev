#!/usr/bin/env python3.11
"""Consistency gate for the CPython 3.11 bytecode support list.

The support list (cinderx/Interpreter/3.11/bytecode_support.toml) is the
single fact source for how the 3.11 JIT front end treats every opcode.  This
checker keeps it honest against the interpreter that actually runs the
product:

  * completeness -- every opcode the anchored CPython 3.11 defines has
    exactly one row, and no row names an opcode that does not exist;
  * cache widths -- each row's inline-cache entry count matches the
    interpreter's own table, because one wrong width desynchronizes every
    later instruction in the stream;
  * normalization -- rows marked "normalize" map to the base opcode the
    interpreter's specialization families define, and only to a base that is
    itself translated;
  * state discipline -- states come from the closed four-value set, refusal
    rows carry a registered reason, translated rows carry none.

Ground truth is the running interpreter's opcode module plus the vendored
pristine opcode_targets.h; the gate runs this under the anchored 3.11.6.
The translator-dispatch consistency check (support list vs. the actual HIR
builder whitelist) becomes possible once the 3.11 JIT front end is compiled
in, and lands with that change.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

STATES = ("translate", "normalize", "refuse", "interpreter-only")

# Reasons are bound to their state: a refusal reason on an
# interpreter-only row (or vice versa) is a classification error, not a
# stylistic choice -- eligibility reporting keys off this split.
REASONS_BY_STATE = {
    "refuse": {
        "REFUSE_PATTERN_MATCHING_UNAUDITED",
        "REFUSE_EXCEPT_STAR_UNAUDITED",
        "REFUSE_HELPER_UNAVAILABLE_PRE314",
        "REFUSE_UNPORTED",
    },
    "interpreter-only": {
        "INTERP_ONLY_ASYNC_CODE",
        "INTERP_ONLY_NON_FUNCTION_SCOPE",
        "INTERP_ONLY_PSEUDO_SLOT",
    },
}

REPO_ROOT = Path(__file__).resolve().parent.parent
SUPPORT_LIST = "cinderx/Interpreter/3.11/bytecode_support.toml"
OPCODE_TARGETS = "cinderx/Interpreter/3.11/upstream/opcode_targets.h"


def parse_targets(path: Path) -> dict[int, str]:
    """Slot -> name table from the vendored pristine opcode_targets.h."""
    names: dict[int, str] = {}
    idx = 0
    for m in re.finditer(r"&&(\w+)", path.read_text()):
        label = m.group(1)
        if label != "_unknown_opcode":
            if not label.startswith("TARGET_"):
                raise SystemExit(f"unexpected label in opcode_targets.h: {label}")
            names[idx] = label[len("TARGET_") :]
        idx += 1
    if idx != 256:
        raise SystemExit(f"opcode_targets.h has {idx} slots, expected 256")
    return names


def ground_truth(repo_root: Path):
    """Name/number/cache/deopt tables from the running 3.11 interpreter."""
    if sys.version_info[:3] != (3, 11, 6):
        raise SystemExit(
            f"must run under the anchored CPython 3.11.6, got {sys.version}"
        )
    import dis
    import opcode

    all_opname = getattr(dis, "_all_opname", opcode.opname)
    defined = {
        i: all_opname[i] for i in range(256) if not all_opname[i].startswith("<")
    }

    targets = parse_targets(repo_root / OPCODE_TARGETS)
    # DO_TRACING is the evaluator's tracing dispatch target: a real slot in
    # opcode_targets.h that is never emitted into co_code and is invisible
    # to the opcode module.  The support list carries it as a pseudo-slot.
    if targets.get(255) == "DO_TRACING":
        defined[255] = "DO_TRACING"
    mismatch = {
        i: (targets.get(i), defined.get(i))
        for i in range(256)
        if targets.get(i) != defined.get(i)
    }
    if mismatch:
        raise SystemExit(
            f"interpreter and vendored opcode tables disagree: {mismatch}"
        )

    caches = list(opcode._inline_cache_entries)
    deopt = {}  # variant name -> base name
    for base, variants in opcode._specializations.items():
        for v in variants:
            deopt[v] = base
    return defined, caches, deopt


def load_doc(path: Path) -> dict:
    import tomllib

    with path.open("rb") as fp:
        doc = tomllib.load(fp)
    if "opcodes" not in doc:
        raise SystemExit(f"{path}: missing [opcodes] table")
    return doc


def load_rows(path: Path) -> dict[str, dict]:
    return load_doc(path)["opcodes"]


def validate_meta(doc: dict) -> list[str]:
    """The [meta] block is the list's anchor contract, not decoration."""
    errors = []
    meta = doc.get("meta", {})
    if meta.get("python") != "3.11.6":
        errors.append(
            f"meta.python is {meta.get('python')!r}, the list is anchored "
            "to '3.11.6'"
        )
    slots = meta.get("slots")
    if type(slots) is not int or slots != 256:
        errors.append(f"meta.slots is {slots!r}, expected the int 256")
    return errors


def validate(rows: dict[str, dict], truth) -> list[str]:
    defined, caches, deopt = truth
    num_of = {n: i for i, n in defined.items()}
    errors: list[str] = []

    for name in sorted(set(defined.values()) - set(rows)):
        errors.append(f"missing row for defined opcode {name}")
    for name in sorted(set(rows) - set(defined.values())):
        errors.append(f"row for unknown opcode {name}")

    for name in sorted(set(rows) & set(defined.values())):
        row = rows[name]
        where = f"opcode {name}"
        state = row.get("state")
        if state not in STATES:
            errors.append(f"{where}: state {state!r} not in {STATES}")
            continue
        if type(row.get("num")) is not int:
            errors.append(f"{where}: num must be an int, got {row.get('num')!r}")
            continue
        if row.get("num") != num_of[name]:
            errors.append(
                f"{where}: num {row.get('num')} != interpreter's {num_of[name]}"
            )
        base = deopt.get(name, name)
        expected_cache = caches[num_of.get(base, num_of[name])]
        if type(row.get("cache")) is not int:
            errors.append(
                f"{where}: cache must be an int, got {row.get('cache')!r}"
            )
        elif row.get("cache") != expected_cache:
            errors.append(
                f"{where}: cache {row.get('cache')} != interpreter's "
                f"{expected_cache}"
            )

        if name in deopt and state != "normalize":
            errors.append(
                f"{where}: the interpreter lists it as a specialization "
                f"variant of {deopt[name]}, so its state must be "
                f"'normalize', not {state!r}"
            )
        if state == "normalize":
            to = row.get("to")
            if to is None:
                errors.append(f"{where}: normalize row without 'to'")
            elif deopt.get(name) != to:
                errors.append(
                    f"{where}: normalizes to {to} but the interpreter's "
                    f"specialization family says {deopt.get(name)}"
                )
            elif rows.get(to, {}).get("state") != "translate":
                errors.append(
                    f"{where}: normalizes to {to}, whose state is "
                    f"{rows.get(to, {}).get('state')!r} instead of 'translate'"
                )
        elif "to" in row:
            errors.append(f"{where}: 'to' is only valid on normalize rows")

        if state in ("refuse", "interpreter-only"):
            reason = row.get("reason")
            if reason not in REASONS_BY_STATE[state]:
                errors.append(
                    f"{where}: {state} row needs a reason registered for "
                    f"that state, got {reason!r}"
                )
        elif "reason" in row:
            errors.append(f"{where}: reason is only valid on refusal rows")

    return errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=REPO_ROOT)
    args = ap.parse_args(argv)

    doc = load_doc(args.repo / SUPPORT_LIST)
    rows = doc["opcodes"]
    errors = validate_meta(doc) + validate(rows, ground_truth(args.repo))
    if errors:
        for err in errors:
            print(f"bytecode-support: {err}", file=sys.stderr)
        print(f"bytecode-support: {len(errors)} error(s)", file=sys.stderr)
        return 1
    counts: dict[str, int] = {}
    for row in rows.values():
        counts[row["state"]] = counts.get(row["state"], 0) + 1
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    print(f"bytecode-support: {len(rows)} opcodes consistent ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
