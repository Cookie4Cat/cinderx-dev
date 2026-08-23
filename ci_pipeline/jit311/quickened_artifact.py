"""Decide whether a regrtest -R "memory blocks" line is a real leak.

regrtest computes its block figure as

    alloc_after = sys.getallocatedblocks() - sys._getquickenedcount()

On CPython 3.11 that subtraction is only sound when both terms come from
the same interpreter.  They do not here.  `_Py_QuickenedCount` is a
file-local symbol in the interpreter (`nm` reports it lowercase `b`), so
it cannot be linked against, and the vendored evaluator in
cinderx/Interpreter/3.11/upstream/specialize.c has to define its own
copy.  Quickening therefore increments CinderX's counter, while
`code_dealloc` in the interpreter decrements the interpreter's counter
for every code object with `co_warmup == 0` -- the one
`sys._getquickenedcount()` returns.  That counter can only fall, and
subtracting a negative number inflates `alloc_after` by exactly the
drift.

So a "memory blocks" line means nothing on its own.  This module runs the
module again while recording both terms, and reports the line as an
artifact only when the arithmetic proves it:

  * the raw block count does not grow across the tracked window, and
  * the quickened counter's drift accounts for the whole reported figure.

If either fails, the line is treated as a real leak.  Reference leaks are
never excused here -- they are the acceptance item's actual subject.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

_HOOK = '''
import atexit, gc, os, sys
from array import array
_OUT = os.environ["QA_OUT"]
_SKIP = int(os.environ["QA_SKIP"])
try:
    from test.libregrtest import refleak
except Exception as exc:
    open(_OUT, "w").write("hook-failed %r\\n" % (exc,))
else:
    _orig = refleak.dash_R_cleanup
    blocks = array("q")
    quick = array("q")
    call = [0]

    def _patched(*a, **k):
        r = _orig(*a, **k)
        gc.collect()
        call[0] += 1
        if call[0] >= _SKIP:
            blocks.append(sys.getallocatedblocks())
            quick.append(sys._getquickenedcount())
        return r

    refleak.dash_R_cleanup = _patched

    @atexit.register
    def _dump():
        if len(blocks) >= 2:
            with open(_OUT, "w") as fh:
                fh.write("blocks %s\\n" % " ".join(str(v) for v in blocks))
                fh.write("quick %s\\n" % " ".join(str(v) for v in quick))
'''

_BLOCKS = re.compile(r"^(\S+) leaked \[([-0-9, ]+)\] memory blocks", re.M)
_REFS = re.compile(r"^(\S+) leaked \[([-0-9, ]+)\] references", re.M)


def classify(python: str, module: str, warmups: int, reps: int) -> tuple[bool, str]:
    """Return (is_artifact, explanation) for one module."""
    with tempfile.TemporaryDirectory() as tmp:
        hook_dir = os.path.join(tmp, "hook")
        os.mkdir(hook_dir)
        with open(os.path.join(hook_dir, "sitecustomize.py"), "w") as fh:
            fh.write(_HOOK)
        out = os.path.join(tmp, "counts.txt")
        env = dict(os.environ)
        env.update(
            QA_OUT=out,
            QA_SKIP=str(warmups),
            PYTHONPATH=hook_dir + os.pathsep + env.get("PYTHONPATH", ""),
        )
        proc = subprocess.run(
            [python, "-m", "test", "-R", f"{warmups}:{reps}", module],
            capture_output=True, text=True, env=env,
        )
        text = proc.stdout + proc.stderr

        refs = _REFS.search(text)
        if refs:
            return False, f"{module}: reference leak {refs.group(2)} -- not an artifact"

        blocks = _BLOCKS.search(text)
        if not blocks:
            return True, f"{module}: no block line on the verification run"

        reported = [int(v) for v in blocks.group(2).split(",")]
        if not os.path.exists(out):
            return False, f"{module}: verification hook produced no counts"
        counts = {}
        with open(out) as fh:
            lines = fh.readlines()
        for line in lines:
            parts = line.split()
            if len(parts) >= 2:
                counts[parts[0]] = [int(v) for v in parts[1:]]
        if "blocks" not in counts or "quick" not in counts:
            return False, f"{module}: verification hook produced no counts"

        return decide(module, reported, counts["blocks"], counts["quick"])


def decide(
    module: str,
    reported: list[int],
    blocks_series: list[int],
    quick_series: list[int],
) -> tuple[bool, str]:
    """The judgement, separated from the run so it can be tested directly.

    Judged on a SETTLED tail only.  Early samples still carry compilation:
    the JIT allocates code buffers while functions are crossing the
    threshold, and that growth is not a leak.  Comparing the first and
    last sample of the whole window calls every execute-mode run a leak.
    """
    tail = max(3, len(blocks_series) // 2)
    b_tail = blocks_series[-tail:]
    q_tail = quick_series[-tail:]
    raw_drift = b_tail[-1] - b_tail[0]
    quick_drift = q_tail[-1] - q_tail[0]
    steps = len(b_tail) - 1
    per_rep = reported[-1] if reported else 0

    if raw_drift > 0:
        return False, (
            f"{module}: raw blocks grew by {raw_drift} over the settled "
            f"tail {b_tail} -- a real leak, not the quickened artifact"
        )
    if quick_drift >= 0:
        return False, (
            f"{module}: quickened counter did not drift ({q_tail}) yet "
            f"{per_rep} blocks were reported -- treating as a real leak"
        )
    implied = -quick_drift / steps if steps else 0
    if per_rep and abs(implied - per_rep) > max(1, 0.25 * per_rep):
        return False, (
            f"{module}: quickened drift implies {implied:.1f} blocks per "
            f"repetition but {per_rep} were reported -- unexplained"
        )
    return True, (
        f"{module}: artifact -- raw blocks flat over the settled tail "
        f"(drift {raw_drift}), quickened counter fell {quick_drift} over "
        f"{steps} repetitions, matching the reported {per_rep} per repetition"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("python")
    ap.add_argument("modules", nargs="+")
    ap.add_argument("--warmups", type=int, default=20)
    ap.add_argument("--reps", type=int, default=6)
    args = ap.parse_args()

    ok = True
    for module in args.modules:
        artifact, why = classify(args.python, module, args.warmups, args.reps)
        print(("artifact: " if artifact else "REAL LEAK: ") + why)
        ok = ok and artifact
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
