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

**The leg currently fails, and it is right to.** Running it revealed a
reference leak on the execute path that had never been visible, because
nothing could build CinderX against a debug interpreter until the
version gate above existed. What is known about it:

* It is confined to execute mode. `off`, `observe` and `shadow` all
  report SUCCESS on the same corpus; only `execute` leaks.
* It is steady-state, not warm-up. `-R 3:6` on `test_bool` reports
  `[15, 18, 18, 18, 18, 18]` -- a constant per-repetition cost with no
  sign of settling.
* **It predates this branch.** Built against the same debug interpreter,
  the MR-10 base (`ca50dc69`) leaks `sum=63` on `test_bool` where this
  branch leaks `sum=51`, and its auto-mode probe leaks three references
  per round to this branch's one. MR-11 does not introduce it and
  measurably reduces it.
* The reference-count matrix and the residency census stay green
  throughout, which places the leak outside both: it is not per-object
  drift on anything a corpus names, and not a native allocation that
  fails to come back.
* It is **per execution of compiled code**, not per compile and not per
  scheduling decision. Instrumenting regrtest's own repetition loop shows
  the JIT's counters flat from the second repetition onward --
  `compiled_function_creations`, `resident_code_buffers`,
  `resident_code_extra_blocks`, `watched_codes` and the compiled-function
  registry all stop moving -- while `machine_code_entries` grows by a
  fixed amount per repetition and the reference total grows with it, at
  roughly 0.06 references per machine-code entry. Nothing is being
  allocated and retained; something INCREFs on an execution path without
  a matching DECREF.
* What gains references are live, long-lived objects: `TextIOWrapper`
  (+2 and +3 per repetition), the test class and `RegressionTestResult`
  types (+2), the `_codecs`, `codecs` and `_signal` modules (+1), and
  some `frozenset`/`set` singletons. No object *type* grows in number,
  which is why the object census cannot see it.
* The smallest reproducer found is an empty test method:
  `python -m test -R 3:3 -m test_nothing <module>` with a
  `unittest.TestCase` whose body is `pass` leaks ten references per
  repetition under `CINDERX_JIT_MODE=execute PYTHONJITAUTO=1`, and none
  under `off`. The leak is entirely in the harness code the JIT compiles;
  the test body is irrelevant.

Two bisection levers were tried and do not work, which is worth knowing
before the next attempt:

* `PYTHONJITLISTFILE` has no effect on this path -- an empty list
  compiles exactly as many functions as no list at all -- so the leaking
  function cannot be isolated by restricting what compiles.
* The `PYTHONJITAUTO` threshold does change whether the leak appears
  (thresholds 1-24 leak, 25 and above are clean on the minimal
  reproducer), but not monotonically, and the leaked amounts swing widely
  between runs at the same threshold (88 to 362 on the same command).
  Diffing the compiled-function sets across the boundary yields five
  candidates -- two of which, `TestResult._setupStdout` and
  `_restoreStdout`, assign `sys.stdout`/`sys.stderr` and would explain
  the `TextIOWrapper` growth -- but compiling each of them alone, with
  nothing else compiled, reproduces no leak. So the boundary is not a
  clean bisection: which functions compile at a given threshold varies
  between runs.

The obvious minimal reproducer -- `exec()` a fresh code object per round
and watch `sys.gettotalrefcount()` -- is useless here: it reports the same
one reference per round with the JIT disabled entirely, because that is
CPython's own per-`exec` growth. Any candidate reproducer has to be
bracketed by mode the way the regrtest corpus is.

It is a real leak, not a `gettotalrefcount` accounting artifact. Measured
together across repetitions, `sys.getallocatedblocks()` grows by five
blocks per repetition while `len(gc.get_objects())` stays exactly flat --
so five untracked allocations are stranded per round, and the fifteen
references belong to objects that really do gain them. Scanning every
reachable object's refcount between repetitions names them: two
`TextIOWrapper`s (+3 and +2), the test class and `RegressionTestResult`
(+2 each), the `_codecs`, `codecs` and `_signal` modules (+1 each), and a
couple of `frozenset`/`set` singletons. Those sum to the fifteen.

Instruments that did NOT localize it, so the next attempt need not repeat
them:

* `tracemalloc`, at depth 20 with its own frames filtered out, reports no
  site growing consistently across rounds -- the stranded blocks are not
  attributable to a Python allocation site.
* Per-operation refcount probes. Compiling a function that does only a
  global load, a module attribute load, a class attribute load, a method
  call, `isinstance`, `type()`, a set membership test, a string concat, a
  tuple index or a slice, then watching the refcount of the exact objects
  involved over thousands of calls, shows zero drift on every one.
* The deopt path. Deopts do correlate in the failing run (exactly four
  per repetition against fifteen references), but forcing thirty-two
  deopts directly produces six references, a tenth of the implied rate,
  so the correlation is two per-repetition constants rather than cause.

The objects that gain references are the ones `TestResult._setupStdout`
and `_restoreStdout` touch, which is the most promising thread left. It
was not possible to confirm it from the outside: those functions are
refused by `force_compile`, so they cannot be compiled in isolation, and
compiling each of the five threshold-boundary candidates alone reproduces
nothing.

The next instrument should be C-level: refcount accounting inside the
generated code and its runtime helpers -- the `JITRT_IncRefTotal` /
`JITRT_DecRefTotal` pair exists precisely because generated code
manipulates refcounts outside the `Py_INCREF` macros, and auditing where
that accounting is and is not paired is a more direct question than any
further black-box bisection.

Fixing it is work in the shared compile and publication machinery, which
3.12 and 3.14 also use, and belongs in its own change rather than inside
the milestone that made it visible.

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
