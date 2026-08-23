# CPython 3.11 execute mode: compatibility deviations

The 3.11 Auto-JIT scheduler is required to be invisible to the program it
runs: reference counts, GC visibility, weak references and object
lifetimes must not differ because the JIT is watching. Most of that is
achievable, and the differential gates enforce it. This file records the
places where it is **not**, so that a known tradeoff is a decision on the
record rather than something a reader has to rediscover.

Each entry states what differs, why the alternative was worse, and what
would remove it.

Entries 1 and 2 are accepted deviations: the behaviour stands, and this
file is where it is recorded. Filing them as formal exceptions in the RFC
was considered and declined by the repository owner, so this file is the
single place they are written down -- a reader comparing the RFC's
"user-visible semantics are unchanged" against the implementation should
come here for the exceptions. Entries 3 and 4 were open in earlier
revisions of this file and are now resolved; they are kept because the
reasoning behind how they were resolved is worth having.

## 1. `code.__sizeof__()` grows for a watched code object

**Accepted deviation.** The first time the scheduler sees a code object it
creates the CinderX code-extra block for it, and CPython 3.11 counts the
`co_extra` allocation in `code.__sizeof__()`. A code object that has run
under `CINDERX_JIT_MODE=observe|shadow|execute` therefore reports a
larger size than the same code object reports under `off` — measured at
200 → 216 bytes for a small function on this platform.

**Why.** The scheduler keeps per-code state in a private table keyed by
address, and an address-keyed table has to know when its keys die. CPython
3.11 has no code watcher, so there are exactly two death signals: a weak
reference, or the `co_extra` free function. A weak reference is worse: it
lives on the referent's weakref list, which `weakref.getweakrefs()` and
`weakref.getweakrefcount()` return to any caller, so observation would be
directly visible to the observed program — and taking such a reference out
of the GC census (which observation requires, because `test_descr` counts
`gc.get_objects()`) also changes what `gc.is_tracked()` reports about the
user's *own* `weakref.ref(code)`, because CPython caches a callback-less
weak reference on its referent and hands the same object back.

The `co_extra` route has a second cost, which is *not* accepted: it forces
`code_dealloc` to walk every foreign code-extra slot below ours. That one
is refused outright — every mode above `off` fails to configure unless
CinderX holds slot zero (see `Ci_Observe311_Configure`).

**What would remove it.** A code-object watcher, which 3.11 does not have.

## 2. Nested artifacts are anchored by a search, not by lexical ownership

**Accepted deviation.** A nested code object's artifact is anchored on its outer
function so it outlives the instance that was compiled, as on 3.12+. The
outer function is *found* — through the module namespace, one level of
class dictionary, exact `staticmethod`/`classmethod` unwrapping, and a
bounded caller-chain walk (`kOuterWalkFrames`, `kOuterWalkNesting`) — and
some shapes are not reachable by any of those. A closure returned by a
decorated factory, where the factory itself survives only in the
decorator's closure cell, is the standard example.

**Consequence.** Bounded and benign: the instance that was compiled keeps
its machine code, and later instances over the same code interpret. No
artifact is ever misattributed, because an anchor is only accepted when
the candidate's own constants contain the code object.

**What would remove it.** Recording the creating function at
`MAKE_FUNCTION`, which means touching the vendored evaluator and the
function layout. Deliberately left as its own change.

## 3. Reference-leak acceptance runs regrtest `-R` on a built interpreter

**Resolved.** The acceptance item names a `refleak` leg, and regrtest's
`-R` needs `sys.gettotalrefcount`, which only a `Py_DEBUG` interpreter
has. The platform ships a release 3.11.6, so `jit311_refleak_execute`
builds one: vanilla CPython 3.11.6 configured `--with-pydebug`, with
CinderX built against it (the wheel carries the `cp311d` ABI tag, so it
cannot be confused with the release build). Both are cached in the run
directory; only the first run pays for them.

Making that build possible needed one fix in CinderX itself: its
`Py_REF_DEBUG` hooks were written against CPython 3.12's spelling
(`interp->object_state.reftotal`, `_Py_INCREF_IncRefTotal`), which 3.11
does not have -- there the total is the process-wide `_Py_RefTotal`. Both
sites are now version-gated. Release builds are unaffected; the code only
exists in a `Py_REF_DEBUG` interpreter.

The leg fails closed on its own premises before reading any leak verdict:
a non-debug interpreter, a CinderX that did not load, an evaluator that
did not install, or a run in which nothing reached machine code all fail
first, because a clean `-R` over an interpreted arm is clean for the
wrong reason.

The residency census (`jit311_lifecycle_census`) stays, and is not
redundant: `-R` measures the process-wide Python reference total, which
says nothing about an executable mapping, a raw code-extra block or the
observer's `calloc()`ed table.

**Running the leg found a real defect, which is now fixed.** Nothing
could build CinderX against a debug interpreter until the version gate
above existed, so the defect had been invisible for the whole port.

### Root cause: the iterator-done sentinel is mortal on 3.11

`JITRT_IterDoneSentinel` (`cinderx/Jit/jit_rt.cpp`) is a statically
allocated `PyObject` used as an address marker: when an iterator is
exhausted, `JITRT_InvokeIterNext` returns its address, and compiled code
recognises the end of the loop by comparing the returned pointer against
that address (`CondBranchIterNotDone` in the LIR generator). Compiled
code never releases it, and the sentinel's other producer,
`JITRT_GenSendHandleStopAsyncIteration`, hands it back without an
incref. Nothing in the tree decrefs it. It is a borrowed marker.

`JITRT_InvokeIterNext` nevertheless did `Py_INCREF` on it. From 3.12 on
the sentinel is initialised with `_Py_IMMORTAL_REFCNT`, so that incref is
a no-op and the imbalance is invisible. On 3.11 the initialiser is a
plain `1` -- the object is **mortal** -- so the incref was a real,
unbalanced increment. **Every `for` loop that ran to exhaustion in
compiled code leaked exactly one reference.**

The fix removes the incref, which changes nothing on 3.12+.

Measured on the micro-reproducer, before and after:

| function | before | after |
| --- | --- | --- |
| one `for` loop | 1.01 refs/call | 0.00 |
| two sequential loops | 2.00 | 0.00 |
| three sequential loops | 3.00 | 0.00 |
| nested (1 outer + 5 inner) | 6.00 | 0.00 |
| loop exited by `break` | 0.00 | 0.00 |
| `while` loop | 0.00 | 0.00 |

`break` does not leak because it never reaches the exhaustion path, and
`while` does not because it never touches the iterator protocol.

### Why it was mistaken for a scheduling defect

The bug is as old as machine-code execution on this port. Steady-state
per-repetition leak, measured per milestone with `-R 30:5` on
`test_grammar -m test_pass_stmt`:

| commit | milestone | refs/rep |
| --- | --- | --- |
| `1aba4064` | M-04, first machine-code execution | 0 |
| `647da465` | M-06 | 0 |
| `6a61af87` | M-07 | 0 |
| `60b6f749` | M-08, exception surface opens | 4 |
| `3328a8cc` | M-09, attribute surface opens | 13 |
| `682d96a0` | M-11 | 13 |

The zeroes are not evidence of a healthy tree: the defect was present at
every one of those commits, and a direct micro-reproducer shows one
reference per loop at all of them. What changed at M-08 and M-09 is the
*compile surface*. Each opened more opcodes, so more functions containing
`for` loops became compilable, and the same bug surfaced more of itself.
Attributing the growth to those merge requests would have been wrong.

Two measurement rules came out of this and are worth keeping:

* **`-R 3:3` cannot compare commits.** Three warm-ups leave compilation
  happening inside the measured repetitions. M-04 reports `[39, 25, 1]`
  at `-R 3:3`, which reads as a leak; at `-R 30:5` its steady state is
  zero. Every number above is a settled floor.
* **A "clean" arm must prove it executed.** Three configuration knobs
  (`IMMORTALIZECOMPILEDFUNCTIONS`, `LIGHTWEIGHTFRAME`,
  `SUPPORTINSTRUMENTATION`) turn the leak off by making the 3.11 gate
  refuse the configuration outright -- `entries=0`, `compiled=0`. Clean
  for the wrong reason.

### Why twelve instruments missed it

The sentinel is a static `PyObject`. It is never registered through
`_Py_NewReference`, so it is absent from the `Py_TRACE_REFS` refchain and
`sys.getobjects()` cannot enumerate it; it is not GC-tracked, so
`gc.get_objects()` cannot see it; and it is never allocated or freed, so
no object census moves. It evades every whole-heap scan simultaneously.
Establishing that -- a complete-heap scan over 68,386 objects finding no
monotone grower while the total climbed 13 per repetition -- was what
finally implicated a static object.

Building the `Py_TRACE_REFS` interpreter that made that scan possible
required fixing a latent compile error: the `#ifdef Py_TRACE_REFS` branch
in `cinderx/Jit/lir/generator.cpp` referenced an undeclared `obj` where
the surrounding code uses `instr`. No CI configuration builds with
trace-refs, so the branch had rotted, and with it the ability to use
whole-heap enumeration as a debugging tool on any Python version.

The other dead end worth recording: every micro-reproducer written
before this used a `while` loop, which never touches `GET_ITER` /
`FOR_ITER`. They all reported clean, correctly, about the wrong thing.

What did localize it was suppressing compiled functions one at a time
(`cinderjit.jit_suppress`) and watching the steady-state number fall.
The three largest contributors -- `BaseTestSuite.addTests`,
`TestSuite.run` and `test.support._filter_suite` -- are all `for` loops,
which named the mechanism.

### Remaining: a memory-block leak in the vendored interpreter

With the sentinel fixed, eight of the ten refleak modules pass and **no
module leaks references any more**. Two still leak memory *blocks*:
`test_listcomps` 8 per repetition and `test_unpack` 3, flat at
`-R 30:5`, with zero reference growth.

That defect is not in the JIT:

| configuration | result |
| --- | --- |
| stock Py_DEBUG CPython 3.11.6 | clean |
| CinderX installed, plugin not enabled (stock evaluator) | clean |
| plugin enabled, `CINDERX_EVAL_MODE=cinder`, **JIT off** | 8 / 3 blocks per repetition |
| same, `shadow` | 8 / 3 |
| same, `execute` | 8 / 3 |

It appears exactly when the vendored 3.11 evaluator is installed and is
identical whether the JIT runs or not, which places it in
`cinderx/Interpreter/3.11/` rather than anywhere MR-11 touches. Ruled
out so far: it is not `CodeExtra` blocks (`resident_code_extra_blocks` is
flat per repetition), and it does not scale with comprehension
execution, unpacking, or `compile()`/`exec()` of fresh code objects --
steady-state per-call block growth for all of those is zero under both
evaluators. It is a fixed cost per test-module repetition.

`jit311_refleak_execute` stays red on that residue, and the leg is left
red rather than narrowed to hide it.

## 4. Import and setup suppression is wired end to end

**Resolved.** Scheduling is withheld while `autoJitImportDepth()` or
`autoJitSetupDepth()` is non-zero, at both scheduling doors -- the
threshold dispatch and fresh attachment, since an already-dispatched code
object reaches the second one on every later frame.

What raises those counters is the Python-side provider, and it used to
turn on only for the `auto[:N]` classifier spelling. CPython 3.11 refuses
that spelling -- its threshold is a plain count -- so the providers were
off in exactly the configuration that schedules. They now key off the
configured mode on 3.11, which is the product configuration and the one
the matrix tests, and an end-to-end test drives a real import to prove
the depth actually rises without any manual `enter`/`leave`.

Enabling them needed one fix. The wrapper installed on
`importlib._bootstrap._find_and_load` becomes a frame in every import, and
`warnings` decides which frame to blame by filename:

```python
def _is_internal_frame(frame):
    filename = frame.f_code.co_filename
    return 'importlib' in filename and '_bootstrap' in filename
```

A wrapper defined in `cinderx/__init__.py` is not skipped, so every
import-time warning was attributed to CinderX. The wrapper is now
compiled under `<frozen importlib._bootstrap>`, which restores the
attribution; the 72-module differential passes with both changes in
place.

One existing probe had to be told which environment it needs.
`test_deopt_sites_pins_artifact_across_reentrant_uncompile` fires its
finalizer by lowering the GC threshold and letting the next tracked
allocation inside `deopt_sites()` collect -- which depends on how many
allocations preceded it. The providers add import machinery of their own
and change that history: with them on, the gen-0 counter sits at 124
rather than 22 and `deopt_sites()` makes no tracked allocation at all, so
nothing collects. What that probe tests is the artifact pin across a
reentrant uncompile, not the providers, so it now pins the environment it
needs rather than depending on the ambient one.
