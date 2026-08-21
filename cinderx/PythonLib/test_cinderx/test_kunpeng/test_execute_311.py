# Copyright (c) Meta Platforms, Inc. and affiliates.

"""The CPython 3.11 execute mode: the product auto-JIT (MR-11).

Every scenario runs in a child interpreter, because the mode is parsed from
the environment when the evaluator is installed.  What this suite holds:

* the four runtime modes answer the same hot function differently -- off
  counts nothing, observe refuses, shadow compiles and discards, execute
  installs and enters machine code;
* one code object gets one scheduling attempt, so a function re-created per
  call never causes a compilation storm, and a failed or refused attempt
  disables automatic compilation of that code object for good (explicit
  force_compile stays available);
* fresh function objects over already-compiled code attach to the published
  artifact within the per-code budget, survive the death of the instance
  that was compiled (the artifact is anchored by the outer function), and
  never attach across namespaces;
* the interpreter's CALL keeps its legal specializations and still enters a
  compiled callee's machine code; a third-party PEP 523 evaluator degrades
  the JIT safely; generators compile on request only; every threshold from
  1 to "armed but never compiling" runs clean; shutdown repeats clean.
"""

import json
import os
import subprocess
import sys
import textwrap
import unittest


def _clean_env():
    # Anything JIT-related inherited from the parent would silently change
    # the child's mode; the child sees exactly what the test sets.
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
    }


PREAMBLE = textwrap.dedent(
    """
    import gc
    import json
    import sys
    import _cinderx, cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()

    # Builtin methods, not Python functions: the helpers must not appear
    # in the counters they read.
    trigger = _cinderx._get_trigger_stats
    observe = _cinderx._get_observe_stats

    def entries():
        return trigger()["machine_code_entries"]

    def creations():
        return trigger()["compiled_function_creations"]

    def events(name):
        return [e for e in observe()["events"] if e["qualname"].endswith(name)]

    def emit(**payload):
        print("JOURNAL " + json.dumps(payload))
    """
)


def run_child(body, *, mode="execute", threshold=30, timeout=120, **env):
    # `body` is one source block or a sequence of them; each block is
    # dedented on its own so the shared snippets above can be spliced in
    # front of an indented test body.
    child_env = _clean_env()
    if mode is not None:
        child_env["CINDERX_JIT_MODE"] = mode
    if threshold is not None:
        child_env["PYTHONJITAUTO"] = str(threshold)
    child_env.update(env)
    blocks = [body] if isinstance(body, str) else list(body)
    source = PREAMBLE + "\n".join(textwrap.dedent(block) for block in blocks)
    source = source.replace("@T@", str(threshold))
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        env=child_env,
        timeout=timeout,
    )


def journal(proc):
    lines = [line for line in proc.stdout.splitlines() if line.startswith("JOURNAL ")]
    if not lines:
        raise AssertionError(
            "child wrote no JOURNAL line\nstdout:\n%s\nstderr:\n%s"
            % (proc.stdout[-2000:], proc.stderr[-2000:])
        )
    return json.loads(lines[-1][len("JOURNAL ") :])


HOT = """
def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total
"""

FACTORY = """
def factory(k):
    def adder(x, y):
        total = x - x
        i = total
        while i < y:
            total = total + x + k
            i = i + 1
        return total
    return adder
"""

BOMB = """
import cinderjit

class Bomb:
    # An unreachable cycle carrying a finalizer: only the collector can
    # free it, so the disable() lands wherever the collector runs rather
    # than between two of the test's own statements.
    def __init__(self):
        self.loop = self

    def __del__(self):
        cinderjit.disable()


def charge(k):
    # Aim the collector at the k-th tracked allocation of whatever runs
    # next.  Warm-up must not collect (the charge has to survive), and the
    # threshold is set relative to the live counter, so the shot lands on a
    # chosen allocation ordinal instead of a chosen wall-clock moment.
    # Sweeping k therefore sweeps every point the decision can land on.
    gc.disable()
    Bomb()
    gc.set_threshold(gc.get_count()[0] + k, 10, 10)
    gc.enable()


def uncharge():
    gc.set_threshold(700, 10, 10)


# Wide enough to cover every tracked allocation the scheduler makes.
SWEEP = 64
"""

NESTED = """
NESTED_SRC = chr(10).join((
    "def outer():",
    "    def inner(a, b):",
    "        total = a - a",
    "        i = total",
    "        while i < b:",
    "            total = total + a",
    "            i = i + 1",
    "        return total",
    "    return inner",
))


def nested_factory(k):
    # A nested code object, reachable through a name in the module
    # namespace: this is the shape whose scheduling has to register an
    # outer function, and the only shape that reaches the code path the
    # sweeps below are aimed at.
    ns = {}
    exec(compile(NESTED_SRC, "<nested%d>" % k, "exec"), globals(), ns)
    globals()["outer%d" % k] = ns["outer"]
    return ns["outer"]
"""


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Execute311Test(unittest.TestCase):
    def run_ok(self, body, **kwargs):
        proc = run_child(body, **kwargs)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return journal(proc)

    # -- the state machine ------------------------------------------------

    def test_mode_matrix_answers_one_hot_function(self):
        body = (
            HOT,
            """
            try:
                import cinderjit
            except ImportError:
                cinderjit = None
            before = trigger()
            values = [hot(i, 4) for i in range(@T@ + 10)]
            after = trigger()
            stats = observe()
            emit(
                values_ok=values == [i * 4 for i in range(@T@ + 10)],
                enabled=stats["enabled"],
                mode=stats["mode"],
                results=[e["result"] for e in events("hot")],
                entries=after["machine_code_entries"] - before["machine_code_entries"],
                creations=after["compiled_function_creations"],
                shadow=after["shadow_compile_success"],
                allocs=after["executable_alloc_calls"],
                cinderjit=cinderjit is not None,
                compiled=(cinderjit is not None and cinderjit.is_jit_compiled(hot)),
            )
            """
        )
        expected = {
            # mode: (enabled, results, entries, creations>0, shadow>0, cinderjit)
            "off": (False, [], 0, False, False, False),
            "observe": (True, ["CINDERX311_JIT_EXEC_DISABLED"], 0, False, False, False),
            "shadow": (True, ["compiled"], 0, False, True, False),
            "execute": (True, ["installed"], 10, True, False, True),
        }
        for mode, (enabled, results, entries, made, shadow, has_cinderjit) in expected.items():
            with self.subTest(mode=mode):
                payload = self.run_ok(body, mode=mode)
                self.assertTrue(payload["values_ok"])
                self.assertEqual(payload["enabled"], enabled)
                self.assertEqual(payload["mode"], mode)
                self.assertEqual(payload["results"], results)
                self.assertEqual(payload["entries"], entries)
                self.assertEqual(payload["creations"] > 0, made)
                self.assertEqual(payload["allocs"] > 0, made)
                self.assertEqual(payload["shadow"] > 0, shadow)
                self.assertEqual(payload["cinderjit"], has_cinderjit)
                self.assertEqual(payload["compiled"], mode == "execute")

    def test_bare_threshold_without_the_mode_executes_nothing(self):
        # The product JIT is opt-in by mode: a threshold alone is the
        # interpreter-only default, with no cinderjit module.
        payload = self.run_ok((
            HOT,
            """
            for i in range(@T@ * 3):
                hot(i, 2)
            try:
                import cinderjit
                has_cinderjit = True
            except ImportError:
                has_cinderjit = False
            emit(
                trigger=trigger(),
                stats=observe(),
                cinderjit=has_cinderjit,
            )
            """),
            mode=None,
        )
        self.assertFalse(payload["stats"]["enabled"])
        self.assertEqual(payload["stats"]["mode"], "off")
        self.assertEqual(payload["stats"]["events"], [])
        self.assertFalse(payload["cinderjit"])
        self.assertTrue(all(v == 0 for v in payload["trigger"].values()))

    # -- one attempt per code object ----------------------------------------

    def test_one_scheduling_attempt_per_code_no_compilation_storm(self):
        # A function re-created on every call is the shape that would
        # otherwise be compiled once per instance.  It is compiled once, the
        # next instances attach within the budget, and the rest run
        # interpreted -- never a second compilation of the same code.
        payload = self.run_ok((
            FACTORY,
            """
            import cinderjit
            compiled_flags = []
            for n in range(200):
                f = factory(n)
                for _ in range(@T@ + 2):
                    assert f(2, 3) == 3 * (2 + n)
                compiled_flags.append(cinderjit.is_jit_compiled(f))
                del f
            gc.collect()
            stats = observe()
            emit(
                events=events("adder"),
                creations=creations(),
                attachments=stats["fresh_attachments"],
                compiled_flags=compiled_flags,
                live=len(cinderjit.get_compiled_functions()),
            )
            """)
        )
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["result"], "installed")
        # The closure's code and the (hot) factory itself: two
        # compilations in two hundred instances.
        self.assertEqual(payload["creations"], 2)
        self.assertEqual(payload["attachments"], 8)
        # The compiled instance, the eight attached ones, then interpreted.
        self.assertEqual(payload["compiled_flags"][:9], [True] * 9)
        self.assertEqual(payload["compiled_flags"][9:], [False] * 191)
        # Only the factory is still compiled; every instance is gone.
        self.assertEqual(payload["live"], 1)

    def test_failed_automatic_attempt_disables_the_code_object(self):
        # A refused automatic attempt records its verdict on the code
        # object: no second attempt through any door, fresh instances are
        # not attached, and the refusal is counted.  force_compile() is the
        # explicit path and is not bound by the verdict.
        payload = self.run_ok(
            """
            import cinderjit
            from cinderx.jit import jit_suppress

            def factory(k):
                @jit_suppress
                def held(x, y):
                    total = x - x
                    i = total
                    while i < y:
                        total = total + x + k
                        i = i + 1
                    return total
                return held

            def held_events():
                return [e for e in observe()["events"]
                        if e["qualname"] == "factory.<locals>.held"]

            disabled_before = observe()["auto_jit_disabled_codes"]
            first = factory(1)
            for _ in range(@T@ + 5):
                first(2, 3)
            after_first = held_events()
            disabled_after_first = observe()["auto_jit_disabled_codes"]
            second = factory(2)
            for _ in range(@T@ + 5):
                second(2, 3)
            stats = observe()
            # The explicit path: lift the suppression and compile by hand.
            cinderjit.jit_unsuppress(second)
            forced = cinderjit.force_compile(second)
            before = entries()
            second(2, 3)
            emit(
                after_first=after_first,
                after_second=held_events(),
                attachments=stats["fresh_attachments"],
                disabled=disabled_after_first - disabled_before,
                disabled_second=stats["auto_jit_disabled_codes"] - disabled_after_first,
                first_compiled=cinderjit.is_jit_compiled(first),
                forced=forced,
                forced_entered=entries() - before,
            )
            """
        )
        self.assertEqual(len(payload["after_first"]), 1)
        self.assertEqual(
            payload["after_first"][0]["result"], "REFUSE_SHAPE_JIT_SUPPRESSED"
        )
        self.assertEqual(payload["after_second"], payload["after_first"])
        self.assertEqual(payload["attachments"], 0)
        self.assertEqual(payload["disabled"], 1)
        self.assertEqual(payload["disabled_second"], 0)
        self.assertFalse(payload["first_compiled"])
        self.assertTrue(payload["forced"])
        self.assertEqual(payload["forced_entered"], 1)

    def test_generator_auto_default_is_off_but_explicit_compile_works(self):
        payload = self.run_ok(
            """
            import cinderjit

            def gen(n):
                i = 0
                while i < n:
                    yield i
                    i = i + 1

            disabled_before = observe()["auto_jit_disabled_codes"]
            for _ in range(@T@ + 3):
                assert list(gen(3)) == [0, 1, 2]
            disabled_after = observe()["auto_jit_disabled_codes"]
            forced = cinderjit.force_compile(gen)
            before = entries()
            assert list(gen(3)) == [0, 1, 2]
            emit(
                events=[e for e in observe()["events"] if e["qualname"] == "gen"],
                disabled=disabled_after - disabled_before,
                forced=forced,
                entered=entries() - before,
            )
            """
        )
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(
            payload["events"][0]["result"], "REFUSE_SHAPE_GENERATOR_AUTO_DISABLED"
        )
        self.assertEqual(payload["disabled"], 1)
        self.assertTrue(payload["forced"])
        self.assertGreater(payload["entered"], 0)

    # -- fresh function objects ----------------------------------------------

    def test_fresh_instances_attach_within_the_budget(self):
        for budget in (8, 3, 0):
            with self.subTest(budget=budget):
                env = {}
                if budget != 8:
                    env["PYTHONJITFRESHATTACHBUDGET"] = str(budget)
                payload = self.run_ok((
                    FACTORY,
                    """
                    import cinderjit
                    kept = []
                    flags = []
                    for n in range(14):
                        f = factory(n)
                        f(2, 3)
                        flags.append(cinderjit.is_jit_compiled(f))
                        kept.append(f)
                    for _ in range(@T@ + 1):
                        kept[0](2, 3)
                    compiled_first = cinderjit.is_jit_compiled(kept[0])
                    later = []
                    for f in kept[1:]:
                        f(2, 3)
                        f(2, 3)
                        later.append(cinderjit.is_jit_compiled(f))
                    stats = observe()
                    before = entries()
                    for f in kept:
                        assert f(2, 3) == 3 * (2 + kept.index(f))
                    emit(
                        compiled_first=compiled_first,
                        later=later,
                        attachments=stats["fresh_attachments"],
                        creations=creations(),
                        entered=entries() - before,
                        same_artifact=len({
                            id(f.__dict__.get("__cinderx_compiled_func__"))
                            for f in kept if cinderjit.is_jit_compiled(f)
                        }),
                    )
                    """),
                    **env,
                )
                self.assertTrue(payload["compiled_first"])
                self.assertEqual(payload["creations"], 1)
                self.assertEqual(payload["attachments"], budget)
                self.assertEqual(payload["later"][:budget], [True] * budget)
                self.assertEqual(payload["later"][budget:], [False] * (13 - budget))
                self.assertEqual(payload["entered"], 1 + budget)
                self.assertEqual(payload["same_artifact"], 1)

    def test_attachment_survives_the_death_of_the_compiled_instance(self):
        # The artifact is anchored by the outer function, so the instance
        # that was compiled can die and a later instance still attaches to
        # the same artifact; dropping the outer function (and every
        # instance) releases the machine code.
        payload = self.run_ok(
            """
            import types
            import cinderjit
            module = types.ModuleType("fresh_owner")
            exec(
                "def factory(k):\\n"
                "    def adder(x, y):\\n"
                "        total = x - x\\n"
                "        i = total\\n"
                "        while i < y:\\n"
                "            total = total + x + k\\n"
                "            i = i + 1\\n"
                "        return total\\n"
                "    return adder\\n",
                module.__dict__,
            )
            sys.modules["fresh_owner"] = module
            resident = cinderjit._get_resident_compiled_functions
            base = resident()
            first = module.factory(1)
            for _ in range(@T@ + 1):
                first(2, 3)
            assert cinderjit.is_jit_compiled(first)
            artifact_id = id(first.__dict__["__cinderx_compiled_func__"])
            anchors = module.factory.__dict__.get(
                "__cinderx_nested_compiled_funcs__", [])
            anchored = any(id(a) == artifact_id for a in anchors)
            del anchors
            del first
            gc.collect()
            # The compiled instance is gone; the outer function keeps the
            # machine code resident.
            resident_after_death = resident() - base

            second = module.factory(2)
            second(2, 3)
            second(2, 3)
            second_compiled = cinderjit.is_jit_compiled(second)
            same = id(second.__dict__.get("__cinderx_compiled_func__")) == artifact_id
            del second
            gc.collect()
            resident_after_second = resident() - base

            del module.factory
            del sys.modules["fresh_owner"]
            del module
            gc.collect()
            emit(
                anchored=anchored,
                resident_after_death=resident_after_death,
                second_compiled=second_compiled,
                same=same,
                resident_after_second=resident_after_second,
                creations=creations(),
                resident=resident() - base,
                attachments=observe()["fresh_attachments"],
            )
            """
        )
        self.assertTrue(payload["anchored"])
        self.assertEqual(payload["resident_after_death"], 1)
        self.assertTrue(payload["second_compiled"])
        self.assertTrue(payload["same"])
        self.assertEqual(payload["resident_after_second"], 1)
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 1)
        # Dropping the outer function released the machine code.
        self.assertEqual(payload["resident"], 0)

    def test_outer_found_through_the_caller_chain(self):
        # A nested function bound nowhere -- the outer is itself local --
        # is anchored on the outermost containing caller.
        payload = self.run_ok(
            """
            import cinderjit

            def outer():
                def make(k):
                    def adder(x, y):
                        total = x - x
                        i = total
                        while i < y:
                            total = total + x + k
                            i = i + 1
                        return total
                    return adder
                flags = []
                for n in range(6):
                    f = make(n)
                    for _ in range(@T@ + 1):
                        f(2, 3)
                    flags.append(cinderjit.is_jit_compiled(f))
                    del f
                    gc.collect()
                return flags, "__cinderx_nested_compiled_funcs__" in outer.__dict__

            flags, anchored = outer()
            emit(flags=flags, anchored=anchored, creations=creations(),
                 attachments=observe()["fresh_attachments"])
            """
        )
        self.assertEqual(payload["flags"], [True] * 6)
        self.assertTrue(payload["anchored"])
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 5)

    def test_namespace_twin_never_attaches(self):
        payload = self.run_ok((
            HOT,
            """
            import types
            import cinderjit
            for i in range(@T@ + 1):
                hot(i, 2)
            assert cinderjit.is_jit_compiled(hot)
            twin = types.FunctionType(
                hot.__code__, {"__builtins__": __builtins__}, "twin")
            for _ in range(5):
                assert twin(2, 3) == 6
            refused = None
            try:
                cinderjit.force_compile(twin)
            except RuntimeError as exc:
                refused = str(exc)
            before = entries()
            twin(2, 3)
            emit(
                twin_compiled=cinderjit.is_jit_compiled(twin),
                refused=refused,
                attachments=observe()["fresh_attachments"],
                entered=entries() - before,
                creations=creations(),
            )
            """)
        )
        self.assertFalse(payload["twin_compiled"])
        self.assertIn("CANNOT_SPECIALIZE", payload["refused"])
        self.assertEqual(payload["attachments"], 0)
        self.assertEqual(payload["entered"], 0)
        self.assertEqual(payload["creations"], 1)

    def test_force_compile_attaches_beyond_the_budget(self):
        # Explicit compilation of a fresh function is never budgeted, and
        # it attaches rather than compiling again.
        payload = self.run_ok((
            FACTORY,
            """
            import cinderjit
            first = factory(1)
            for _ in range(@T@ + 1):
                first(2, 3)
            fresh = factory(2)
            fresh(2, 3)
            fresh(2, 3)
            auto_attached = cinderjit.is_jit_compiled(fresh)
            forced = cinderjit.force_compile(fresh)
            before = entries()
            fresh(2, 3)
            emit(
                auto_attached=auto_attached,
                forced=forced,
                compiled=cinderjit.is_jit_compiled(fresh),
                entered=entries() - before,
                creations=creations(),
                attachments=observe()["fresh_attachments"],
            )
            """),
            PYTHONJITFRESHATTACHBUDGET="0",
        )
        self.assertFalse(payload["auto_attached"])
        self.assertTrue(payload["forced"])
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 0)

    def test_paused_jit_and_tracing_defer_attachment(self):
        payload = self.run_ok((
            FACTORY,
            """
            import cinderjit
            first = factory(1)
            for _ in range(@T@ + 1):
                first(2, 3)
            assert cinderjit.is_jit_compiled(first)

            cinderjit.disable()
            paused = factory(2)
            paused(2, 3)
            paused(2, 3)
            paused_attached = cinderjit.is_jit_compiled(paused)
            cinderjit.enable()
            paused(2, 3)
            resumed_attached = cinderjit.is_jit_compiled(paused)

            traced = factory(3)
            sys.settrace(lambda *a: None)
            traced(2, 3)
            traced(2, 3)
            sys.settrace(None)
            traced_attached = cinderjit.is_jit_compiled(traced)
            traced(2, 3)
            after_trace_attached = cinderjit.is_jit_compiled(traced)
            emit(
                paused_attached=paused_attached,
                resumed_attached=resumed_attached,
                traced_attached=traced_attached,
                after_trace_attached=after_trace_attached,
                attachments=observe()["fresh_attachments"],
                creations=creations(),
            )
            """)
        )
        self.assertFalse(payload["paused_attached"])
        self.assertTrue(payload["resumed_attached"])
        self.assertFalse(payload["traced_attached"])
        self.assertTrue(payload["after_trace_attached"])
        self.assertEqual(payload["attachments"], 2)
        self.assertEqual(payload["creations"], 1)

    def test_dispatch_is_deferred_while_tracing(self):
        # A function whose threshold crossing happens under a trace
        # function is not scheduled there: a publication made on a traced
        # thread would be reported as a failed attempt and disable the
        # code object for good.  Counting continues, and the first untraced
        # frame at or past the threshold dispatches and installs.  A
        # function that only ever runs traced stays interpreted.
        payload = self.run_ok((
            HOT,
            """
            import cinderjit

            def tracer(frame, event, arg):
                return tracer

            sys.settrace(tracer)
            for i in range(@T@ * 2):
                assert hot(i, 2) == i * 2
            traced_events = events("hot")
            traced_compiled = cinderjit.is_jit_compiled(hot)
            sys.settrace(None)
            disabled_before = observe()["auto_jit_disabled_codes"]
            assert hot(3, 2) == 6
            untraced_events = events("hot")
            before = entries()
            assert hot(3, 2) == 6
            emit(
                traced_events=traced_events,
                traced_compiled=traced_compiled,
                untraced_events=untraced_events,
                compiled=cinderjit.is_jit_compiled(hot),
                entered=entries() - before,
                disabled=observe()["auto_jit_disabled_codes"] - disabled_before,
            )
            """),
        )
        self.assertEqual(payload["traced_events"], [])
        self.assertFalse(payload["traced_compiled"])
        self.assertEqual(len(payload["untraced_events"]), 1)
        # The crossing is recorded at the count the dispatch happened at.
        self.assertEqual(payload["untraced_events"][0]["count"], 2 * 30 + 1)
        self.assertEqual(payload["untraced_events"][0]["result"], "installed")
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)
        self.assertEqual(payload["disabled"], 0)

    def test_dispatch_is_deferred_while_the_jit_is_paused(self):
        # A code object gets one automatic attempt, so it must not be spent
        # on a paused JIT: every request is refused while paused, and the
        # one-attempt rule would make that refusal permanent.  Counting
        # continues and the first frame after enable() dispatches.
        payload = self.run_ok((
            HOT,
            """
            import cinderjit

            cinderjit.disable()
            for i in range(@T@ * 2):
                assert hot(i, 2) == i * 2
            paused_events = events("hot")
            paused_compiled = cinderjit.is_jit_compiled(hot)
            cinderjit.enable()
            assert hot(3, 2) == 6
            after_events = events("hot")
            before = entries()
            assert hot(3, 2) == 6
            emit(
                paused_events=paused_events,
                paused_compiled=paused_compiled,
                after_events=after_events,
                compiled=cinderjit.is_jit_compiled(hot),
                entered=entries() - before,
            )
            """),
        )
        self.assertEqual(payload["paused_events"], [])
        self.assertFalse(payload["paused_compiled"])
        self.assertEqual(len(payload["after_events"]), 1)
        self.assertEqual(payload["after_events"][0]["result"], "installed")
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)

    def test_a_foreign_twin_compiles_once_the_owner_retires(self):
        # Sharing is by namespace: a function built over compiled code but
        # given globals of its own is not a member, so it is refused while
        # the artifact has an owner.  The refusal describes the artifact's
        # occupancy, not the code object, and it lasts exactly as long as
        # that occupancy -- retiring the owner makes the same twin
        # compilable.  Both compiles here are explicit; the automatic
        # attempt is not involved either way.
        payload = self.run_ok((
            HOT,
            """
            import types
            import cinderjit

            for i in range(@T@ + 1):
                hot(i, 2)
            assert cinderjit.is_jit_compiled(hot)

            twin = types.FunctionType(
                hot.__code__, {"__builtins__": __builtins__}, "twin")
            refused = None
            try:
                cinderjit.force_compile(twin)
            except RuntimeError as exc:
                refused = str(exc)

            # Retire the first owner, then the twin may compile.
            assert cinderjit.force_uncompile(hot) is True
            forced = cinderjit.force_compile(twin)
            before = entries()
            assert twin(2, 3) == 6
            emit(
                refused=refused,
                forced=forced,
                compiled=cinderjit.is_jit_compiled(twin),
                entered=entries() - before,
            )
            """),
        )
        self.assertIn("CANNOT_SPECIALIZE", payload["refused"])
        self.assertTrue(payload["forced"])
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)

    def test_uncompiling_one_member_releases_the_shared_code_buffer(self):
        # Retirement is by artifact: uncompiling one member takes the
        # artifact away from every member, so every member's anchor must go
        # with it.  An anchor left on a sibling would hold the code buffer
        # resident with nothing able to reach it -- the registries naming it
        # are gone, and force_uncompile() on that sibling finds no
        # compilation state and returns without touching the anchor.
        payload = self.run_ok((
            FACTORY,
            """
            import cinderjit
            resident = cinderjit._get_resident_compiled_functions

            base = resident()
            members = [factory(n) for n in range(3)]
            for f in members:
                assert cinderjit.force_compile(f) is True
            assert all(cinderjit.is_jit_compiled(f) for f in members)
            anchored_before = [
                "__cinderx_compiled_func__" in f.__dict__ for f in members]
            resident_compiled = resident() - base

            assert cinderjit.force_uncompile(members[1]) is True
            emit(
                anchored_before=anchored_before,
                resident_compiled=resident_compiled,
                creations=creations(),
                compiled_after=[cinderjit.is_jit_compiled(f) for f in members],
                anchored_after=[
                    "__cinderx_compiled_func__" in f.__dict__ for f in members],
                resident_after=resident() - base,
                values_ok=[f(2, 3) for f in members] == [
                    3 * (2 + n) for n in range(3)],
            )
            """),
        )
        # One artifact, three owning members.
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["anchored_before"], [True] * 3)
        self.assertEqual(payload["resident_compiled"], 1)
        # Retirement reaches every member, anchors included.
        self.assertEqual(payload["compiled_after"], [False] * 3)
        self.assertEqual(payload["anchored_after"], [False] * 3)
        self.assertEqual(payload["resident_after"], 0)
        # ...and the members still compute the same answers interpreted.
        self.assertTrue(payload["values_ok"])

    def test_a_twin_refusal_spends_the_dispatch_but_not_the_code(self):
        # One attempt per code object, and this twin spends it: the slot
        # is marked dispatched and nothing reschedules it, even after the
        # thing it was refused over goes away.
        #
        # The verdict on the CODE OBJECT is a different question, and the
        # answer here is no.  This refusal is about the function -- its
        # globals are not the artifact's -- so recording it against the
        # code would condemn every other instance of that code, including
        # the ones the artifact was published for.  The two layers are
        # deliberately not the same layer.
        payload = self.run_ok((
            HOT,
            """
            import types
            import cinderjit

            owner = hot
            assert cinderjit.force_compile(owner) is True
            twin = types.FunctionType(
                owner.__code__, {"__builtins__": __builtins__}, "twin")

            def code_events():
                return [e["result"] for e in observe()["events"]
                        if e["qualname"] == "hot"]

            # Process-wide counter: measure the window, not the total.
            disabled_before = observe()["auto_jit_disabled_codes"]
            for _ in range(@T@ + 1):
                twin(2, 3)
            refused = code_events()
            disabled = observe()["auto_jit_disabled_codes"] - disabled_before

            # Remove what the refusal was about, then keep the twin hot for
            # a long time: the attempt was spent, so nothing reschedules.
            assert cinderjit.force_uncompile(owner) is True
            before = entries()
            for _ in range(@T@ * 8):
                assert twin(2, 3) == 6
            emit(
                refused=refused,
                after=code_events(),
                disabled=disabled,
                compiled=cinderjit.is_jit_compiled(twin),
                entered=entries() - before,
            )
            """),
        )
        self.assertEqual(
            payload["refused"], ["REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED"]
        )
        # Exactly one attempt, ever.
        self.assertEqual(payload["after"], payload["refused"])
        # ...and the code object's own verdict agrees with the scheduler.
        # The dispatch was spent, so nothing reschedules ...
        self.assertEqual(payload["disabled"], 0)
        self.assertFalse(payload["compiled"])
        self.assertEqual(payload["entered"], 0)

    def test_transient_conditions_never_reach_the_attempt(self):
        # The one-attempt rule is only safe because conditions that say
        # nothing about the code object are held back BEFORE the attempt
        # is spent.  Tracing and a paused JIT are the two that occur in
        # practice; both must leave the code object schedulable.
        for setup, teardown in (
            ("sys.settrace(lambda *a: None)", "sys.settrace(None)"),
            ("cinderjit.disable()", "cinderjit.enable()"),
        ):
            with self.subTest(setup=setup):
                payload = self.run_ok((
                    HOT,
                    """
                    import cinderjit
                    # Process-wide counter: measure the window, not the
                    # total (the preamble's imports moved it already).
                    disabled_before = observe()["auto_jit_disabled_codes"]
                    @SETUP@
                    for i in range(@T@ * 3):
                        hot(i, 2)
                    held = [e["result"] for e in events("hot")]
                    disabled_during = (
                        observe()["auto_jit_disabled_codes"] - disabled_before)
                    @TEARDOWN@
                    assert hot(3, 2) == 6
                    before = entries()
                    assert hot(3, 2) == 6
                    emit(
                        held=held,
                        disabled_during=disabled_during,
                        after=[e["result"] for e in events("hot")],
                        compiled=cinderjit.is_jit_compiled(hot),
                        entered=entries() - before,
                    )
                    """.replace("@SETUP@", setup).replace("@TEARDOWN@", teardown)),
                )
                # Nothing was dispatched and nothing was recorded while the
                # condition held.
                self.assertEqual(payload["held"], [])
                self.assertEqual(payload["disabled_during"], 0)
                # The attempt was still there once the condition passed.
                self.assertEqual(payload["after"], ["installed"])
                self.assertTrue(payload["compiled"])
                self.assertEqual(payload["entered"], 1)

    def test_staticmethod_and_classmethod_factories_anchor_their_closures(self):
        # A factory bound as a staticmethod or classmethod is a wrapper
        # object in the class dictionary, not a function.  Without
        # unwrapping it the closure's artifact has no outer to live on, so
        # it dies with the first instance and every later instance runs
        # interpreted for good -- the factory has already returned by the
        # time the closure gets hot, so the caller chain cannot help.
        for kind in ("staticmethod", "classmethod"):
            with self.subTest(kind=kind):
                payload = self.run_ok(
                    """
                    import cinderjit
                    resident = cinderjit._get_resident_compiled_functions

                    class C:
                        @KIND@
                        def factory(*args):
                            k = args[-1]
                            def hot(x, y):
                                t = x - x
                                i = t
                                while i < y:
                                    t = t + x + k
                                    i = i + 1
                                return t
                            return hot

                    base = resident()
                    first = C.factory(1)
                    for _ in range(@T@ + 1):
                        first(2, 3)
                    first_compiled = cinderjit.is_jit_compiled(first)
                    del first
                    gc.collect()
                    resident_after_death = resident() - base

                    second = C.factory(2)
                    second(2, 3)
                    second(2, 3)
                    emit(
                        first_compiled=first_compiled,
                        resident_after_death=resident_after_death,
                        second_compiled=cinderjit.is_jit_compiled(second),
                        creations=creations(),
                        values_ok=second(2, 3) == 3 * (2 + 2),
                    )
                    """.replace("@KIND@", kind),
                )
                self.assertTrue(payload["first_compiled"])
                # The artifact outlived the instance that was compiled.
                self.assertEqual(payload["resident_after_death"], 1)
                self.assertTrue(payload["second_compiled"])
                self.assertEqual(payload["creations"], 1)
                self.assertTrue(payload["values_ok"])

    def test_threshold_rejects_negative_and_overflowing_values(self):
        # strtoull() accepts a sign, so an unchecked parse turns "-1" into
        # a threshold no program reaches: auto-JIT reports itself on and
        # never compiles anything.  Both that and a range error must be
        # refused outright.
        from test.support.script_helper import assert_python_failure

        for value in ("-1", "99999999999999999999999999", "0", "1.5", " 5"):
            with self.subTest(value=value):
                env = _clean_env()
                env["CINDERX_JIT_MODE"] = "execute"
                env["PYTHONJITAUTO"] = value
                proc = subprocess.run(
                    [
                        sys.executable,
                        "-c",
                        "import _cinderx, cinderx\n"
                        "cinderx.init()\n"
                        "_cinderx.install_frame_evaluator()\n",
                    ],
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=60,
                )
                self.assertNotEqual(proc.returncode, 0, value)
                self.assertIn("positive integer", proc.stderr)

    def test_attach_budget_above_the_counter_width_is_clamped(self):
        # The per-code attach counter is 16 bits and saturates.  A budget
        # beyond that could never be reached, which would turn the cap into
        # no cap at all; the configuration is clamped instead.
        payload = self.run_ok((
            FACTORY,
            """
            import cinderjit
            kept = []
            for n in range(12):
                f = factory(n)
                f(2, 3)
                f(2, 3)
                kept.append(cinderjit.is_jit_compiled(f))
            emit(kept=kept, attachments=observe()["fresh_attachments"])
            """),
            threshold=1,
            PYTHONJITFRESHATTACHBUDGET="70000",
        )
        # Clamped to the counter width, so attachment still terminates.
        self.assertLessEqual(payload["attachments"], 0xFFFF)
        self.assertTrue(any(payload["kept"]))

    # -- CALL specialization and the evaluator ------------------------------

    def test_interpreter_call_keeps_legal_specialization_and_enters_callee(self):
        # The caller stays interpreted (it never crosses the threshold); its
        # CALL sites specialize as stock would without a PEP 523 hook
        # (introspection parity), the builtin fast path is kept, and every
        # call of the compiled callee enters machine code: under our
        # evaluator the Python-callee fast path goes through the callee's
        # vectorcall entry rather than pushing the frame inline.
        payload = self.run_ok((
            HOT,
            """
            import dis
            import cinderjit

            def caller(n, xs):
                acc = 0
                for i in range(n):
                    acc = acc + hot(i, 3) + len(xs)
                return acc

            assert cinderjit.force_compile(hot) is True
            before = entries()
            value = caller(200, [1, 2, 3])
            ops = sorted({
                i.opname for i in dis.get_instructions(caller, adaptive=True)
                if i.opname.startswith(("CALL", "PRECALL"))
            })
            emit(
                value=value,
                entered=entries() - before,
                caller_compiled=cinderjit.is_jit_compiled(caller),
                ops=ops,
            )
            """),
            threshold=1000000,
        )
        self.assertEqual(payload["value"], sum(i * 3 + 3 for i in range(200)))
        self.assertEqual(payload["entered"], 200)
        self.assertFalse(payload["caller_compiled"])
        self.assertIn("PRECALL_NO_KW_LEN", payload["ops"])
        self.assertIn("CALL_PY_EXACT_ARGS", payload["ops"])

    def test_third_party_evaluator_degrades_the_jit_safely(self):
        payload = self.run_ok((
            HOT,
            """
            import ctypes
            import cinderjit
            assert cinderjit.force_compile(hot) is True
            api = ctypes.pythonapi
            api.PyInterpreterState_Get.restype = ctypes.c_void_p
            api._PyInterpreterState_SetEvalFrameFunc.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p]
            interp = api.PyInterpreterState_Get()
            stock = ctypes.cast(api._PyEval_EvalFrameDefault, ctypes.c_void_p).value

            before = entries()
            assert hot(2, 3) == 6
            entered_ours = entries() - before

            # Another PEP 523 client takes the slot (the stock evaluator
            # stands in for it): nothing runs compiled, nothing crashes,
            # and we do not write our pointer back over theirs.
            api._PyInterpreterState_SetEvalFrameFunc(interp, stock)
            installed_foreign = _cinderx.is_frame_evaluator_installed()
            compiled_foreign = cinderjit.is_jit_compiled(hot)
            before = entries()
            values = [hot(i, 3) for i in range(@T@ + 5)]
            entered_foreign = entries() - before
            foreign_events = len(events("hot"))
            removal = None
            try:
                _cinderx.remove_frame_evaluator()
            except RuntimeError as exc:
                removal = str(exc)

            # They give the slot back; we take it again and machine code
            # resumes from the same published artifact.
            _cinderx.install_frame_evaluator()
            before = entries()
            assert hot(2, 3) == 6
            emit(
                entered_ours=entered_ours,
                installed_foreign=installed_foreign,
                compiled_foreign=compiled_foreign,
                values_ok=values == [i * 3 for i in range(@T@ + 5)],
                entered_foreign=entered_foreign,
                foreign_events=foreign_events,
                removal=removal,
                installed_again=_cinderx.is_frame_evaluator_installed(),
                compiled_again=cinderjit.is_jit_compiled(hot),
                entered_again=entries() - before,
            )
            """)
        )
        self.assertEqual(payload["entered_ours"], 1)
        self.assertFalse(payload["installed_foreign"])
        self.assertFalse(payload["compiled_foreign"])
        self.assertTrue(payload["values_ok"])
        self.assertEqual(payload["entered_foreign"], 0)
        # No frame of ours ran, so nothing was counted or scheduled.
        self.assertEqual(payload["foreign_events"], 0)
        self.assertIn("another component replaced", payload["removal"])
        self.assertTrue(payload["installed_again"])
        self.assertTrue(payload["compiled_again"])
        self.assertEqual(payload["entered_again"], 1)

    # -- thresholds and shutdown ---------------------------------------------

    def test_threshold_matrix(self):
        body = (
            HOT,
            """
            try:
                import cinderjit
            except ImportError:
                cinderjit = None
            # Bracket the loop with the builtin counter reads: at a low
            # threshold everything that ran since the evaluator was
            # installed (the import machinery's helpers, os.environ's
            # codecs) is compiled too and enters machine code of its own.
            before = trigger()["machine_code_entries"]
            values = [hot(i, 2) for i in range(200)]
            hot_entries = trigger()["machine_code_entries"] - before
            stats = observe()
            emit(
                values_ok=values == [i * 2 for i in range(200)],
                threshold=stats["threshold"],
                events=[e for e in stats["events"] if e["qualname"] == "hot"],
                entries=hot_entries,
                installed=_cinderx.is_frame_evaluator_installed(),
                mode=stats["mode"],
                compiled=cinderjit is not None and cinderjit.is_jit_compiled(hot),
            )
            """
        )
        # (PYTHONJITAUTO, effective threshold, expected installs)
        cases = [("1", 1), ("2", 2), ("4", 4), (None, 50), ("1000000000", None)]
        for raw, effective in cases:
            with self.subTest(threshold=raw):
                payload = self.run_ok(body, threshold=raw)
                self.assertTrue(payload["values_ok"])
                self.assertTrue(payload["installed"])
                self.assertEqual(payload["mode"], "execute")
                if effective is None:
                    # Armed but never compiling: the interpreted control arm.
                    self.assertEqual(payload["events"], [])
                    self.assertEqual(payload["entries"], 0)
                    self.assertFalse(payload["compiled"])
                    continue
                self.assertEqual(payload["threshold"], effective)
                self.assertEqual(len(payload["events"]), 1)
                self.assertEqual(payload["events"][0]["count"], effective)
                self.assertEqual(payload["events"][0]["result"], "installed")
                # hot's own calls after the threshold crossing.  The
                # comprehension driving them is scheduled at its first
                # (and only) frame at threshold 1, which is already in the
                # interpreter, so it never enters machine code.
                self.assertEqual(payload["entries"], 200 - effective)
                self.assertTrue(payload["compiled"])

    def test_shutdown_repeats_clean_with_live_state(self):
        # Compiled functions, attached fresh instances, suspended compiled
        # generators and a parked function are all left alive at exit.
        body = (
            FACTORY,
            HOT,
            """
            import cinderjit
            kept = [factory(n) for n in range(6)]
            for f in kept:
                for _ in range(@T@ + 1):
                    f(2, 3)
            for _ in range(@T@ + 1):
                hot(2, 3)

            def gen(n):
                i = 0
                while i < n:
                    yield i
                    i = i + 1

            assert cinderjit.force_compile(gen) is True
            suspended = gen(5)
            next(suspended)
            # A parked function: uncompiled, its artifact retired.
            assert cinderjit.force_uncompile(hot) is True
            emit(
                live=len(cinderjit.get_compiled_functions()),
                attachments=observe()["fresh_attachments"],
            )
            """
        )
        for repetition in range(10):
            with self.subTest(repetition=repetition):
                proc = run_child(body)
                self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
                self.assertEqual(proc.stderr, "")
                payload = journal(proc)
                # Six closure instances and the generator.
                self.assertEqual(payload["live"], 7)
                self.assertEqual(payload["attachments"], 5)



    def test_observation_leaves_user_weak_references_as_stock_leaves_them(self):
        # The observer holds a weak reference per observed code object and
        # takes it out of the GC census, because observation has to stay
        # invisible to gc.get_objects() -- test_descr literally counts it.
        # CPython caches a callback-less weak reference ON its referent and
        # hands the same object back to the next weakref.ref(), so an
        # observer using one would be untracking an object user code can
        # reach: gc.is_tracked() and gc.get_objects() would then answer
        # differently about the user's own weakref.ref(fn.__code__)
        # depending on whether the JIT was watching.  The JIT-off arm is
        # the oracle, and the two must agree.
        body = """
            import gc
            import weakref

            def target(a, b):
                return a + b

            for i in range(@T@ * 2):
                assert target(i, 2) == i + 2

            ref = weakref.ref(target.__code__)
            emit(
                # The referent's weakref list, which weakref.getweakrefs()
                # hands to any caller verbatim: a handle the observer put
                # there would be visible to the program being observed, no
                # matter what callback it carried.
                listed=weakref.getweakrefcount(target.__code__),
                tracked=gc.is_tracked(ref),
                censused=any(obj is ref for obj in gc.get_objects()),
                # CPython's cache: asking twice gives the same object.
                cached=(weakref.ref(target.__code__) is ref),
                # Weak references to code objects that the census can
                # see.  The observer holds one per observed code object;
                # none of them may appear here.  Counting only this
                # referent type keeps the JIT's other weak references --
                # the function death watch, the type watchers -- out of a
                # measurement that is about the observer.
                code_refs=sum(
                    1 for obj in gc.get_objects()
                    if type(obj) is weakref.ref
                    and type(obj()) is type(target.__code__)
                ),
                observed=observe()["codes_seen"],
            )
            """
        off = self.run_ok(body, mode="off")
        execute = self.run_ok(body, mode="execute")
        # A user weak reference is an ordinary tracked object either way.
        self.assertTrue(off["tracked"])
        self.assertTrue(off["censused"])
        self.assertTrue(off["cached"])
        # Stock puts exactly the one the test made on the list.
        self.assertEqual(off["listed"], 1)
        for key in ("listed", "tracked", "censused", "cached"):
            self.assertEqual(execute[key], off[key], key)
        # And the observer's own handles stay out of the census, which is
        # what the untracking is for: watching many code objects must not
        # add a single code weak reference to what gc.get_objects()
        # reports.  The only one either arm can see is the user's.
        self.assertEqual(off["code_refs"], 1)
        self.assertEqual(execute["code_refs"], off["code_refs"])
        # The observer was watching -- otherwise there would be nothing to
        # hide and the assertion above would pass vacuously.
        self.assertGreater(execute["observed"], 1)
        self.assertEqual(off["observed"], 0)

    def test_watching_a_code_object_ends_when_the_code_object_dies(self):
        # The observer keeps one table entry per code object it has seen
        # and holds no reference to any of them, so the entry has to be
        # retired by the death notice rather than by a weak reference
        # clearing.  A missed notice would leave the entry keyed to an
        # address that a later code object can be allocated at, which would
        # hand that code object a used-up attempt.
        payload = self.run_ok(
            """
            import gc

            SRC = chr(10).join(("def f(a, b):", "    return a + b", ""))

            def burn(n):
                ns = {}
                for k in range(n):
                    exec(compile(SRC, "<burn%d>" % k, "exec"), ns, ns)
                    for i in range(@T@ + 5):
                        assert ns["f"](i, 2) == i + 2

            burn(50)
            gc.collect()
            base = observe()
            rounds = []
            for _ in range(4):
                burn(200)
                gc.collect()
                now = observe()
                rounds.append((now["watched_codes"], now["table_capacity"],
                               now["codes_seen"]))
            emit(base=[base["watched_codes"], base["table_capacity"],
                       base["codes_seen"]], rounds=rounds)
            """
        )
        watched = [row[0] for row in payload["rounds"]]
        capacity = [row[1] for row in payload["rounds"]]
        seen = [row[2] for row in payload["rounds"]]
        # 800 more code objects were observed across the four rounds ...
        self.assertGreater(seen[-1] - payload["base"][2], 700)
        # ... and not one of them is still being watched: every entry was
        # retired by its death notice.
        self.assertEqual(watched, [payload["base"][0]] * len(watched))
        # The table therefore never had to grow to hold them.
        self.assertEqual(capacity, [payload["base"][1]] * len(capacity))


    def test_the_observer_table_does_not_ratchet_across_churn_cycles(self):
        # A slot keeps its key after its code object dies, because cutting
        # the key would cut every probe chain running through it.  So a
        # workload that observes many short-lived code objects fills the
        # table with tombstones and trips the load factor with almost
        # nothing alive in it -- and rehashing by doubling there buys
        # memory to hold entries that are already garbage, one octave per
        # churn cycle, while the live population stays flat.  The table is
        # a calloc() allocation sized by capacity, so watched_codes coming
        # back to baseline does not by itself say the memory did.
        #
        # Each round holds its code objects alive together, so their
        # addresses cannot be recycled and the table has to hold every one
        # of them; then drops them all.
        payload = self.run_ok(
            """
            import gc

            SRC = chr(10).join(("def f(a, b):", "    return a + b", ""))

            def burn(n):
                live = []
                for k in range(n):
                    ns = {}
                    exec(compile(SRC, "<t%d>" % k, "exec"), ns, ns)
                    live.append(ns["f"])
                    for i in range(@T@ + 5):
                        assert ns["f"](i, 2) == i + 2
                return live

            rounds = []
            for _ in range(6):
                live = burn(700)
                peak = observe()
                del live
                gc.collect()
                rest = observe()
                rounds.append((peak["watched_codes"], peak["table_capacity"],
                               rest["watched_codes"], rest["table_capacity"],
                               rest["codes_seen"]))
            emit(rounds=rounds)
            """
        )
        rounds = payload["rounds"]
        peak_watched = [r[0] for r in rounds]
        capacity = [r[1] for r in rounds]
        resting = [r[2] for r in rounds]
        # The workload is identical every round: same live population at
        # the peak, same population left behind.
        self.assertEqual(peak_watched, [peak_watched[0]] * len(peak_watched))
        self.assertEqual(resting, [resting[0]] * len(resting))
        self.assertGreater(peak_watched[0], 600)
        # 4200 code objects were observed across the six rounds ...
        self.assertGreater(rounds[-1][4], 4000)
        # ... and the table never grew past what one round's live
        # population actually needs.
        self.assertEqual(capacity, [capacity[0]] * len(capacity))


    def test_a_foreign_code_extra_slot_below_ours_refuses_the_mode(self):
        # Watching a code object means storing into its co_extra, and
        # CPython 3.11's code_dealloc then walks slots 0..ce_size calling
        # every registered free function -- foreign slots included, for
        # code objects that never populated them.  Capping what we
        # allocate protects the slots above ours; nothing protects the
        # ones below, and a mortal foreign free function (test.test_code
        # registers a ctypes callback at import) can already be dead when
        # the last code objects are torn down, which turns that walk into
        # a jump through a freed trampoline.
        #
        # Registration order is a load-order accident, so the mode has to
        # refuse rather than hope.  A normal startup claims slot 0 long
        # before user code runs, which is why every other test here works.
        body = """
            import ctypes

            CALLS = []
            FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_voidp)

            def _free(ptr):
                CALLS.append(ptr)

            _callback = FREEFUNC(_free)
            capi = ctypes.pythonapi
            capi._PyEval_RequestCodeExtraIndex.restype = ctypes.c_ssize_t
            foreign = capi._PyEval_RequestCodeExtraIndex(_callback)

            refused = None
            try:
                import _cinderx, cinderx
                cinderx.init()
                _cinderx.install_frame_evaluator()
            except RuntimeError as exc:
                refused = str(exc)

            def target(a, b):
                return a + b

            for i in range(@T@ * 2):
                assert target(i, 2) == i + 2

            import json
            print("JOURNAL " + json.dumps({
                "foreign": foreign,
                "refused": refused,
                "foreign_calls": len(CALLS),
            }))
            """
        # Spawned without the shared preamble, which imports CinderX on
        # the first line: the whole point is to register the foreign slot
        # before that import happens.
        def run_raw(mode):
            env = _clean_env()
            env["CINDERX_JIT_MODE"] = mode
            env["PYTHONJITAUTO"] = "30"
            return subprocess.run(
                [sys.executable, "-c", textwrap.dedent(body).replace("@T@", "30")],
                capture_output=True,
                text=True,
                env=env,
                timeout=120,
            )

        for mode in ("observe", "shadow", "execute"):
            with self.subTest(mode=mode):
                proc = run_raw(mode)
                self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
                payload = journal(proc)
                # The foreign component really did land below us.
                self.assertEqual(payload["foreign"], 0)
                # And the mode refused rather than running.
                self.assertIsNotNone(payload["refused"])
                self.assertIn("code-extra slot", payload["refused"])
                # Nothing was watched, so nothing forced the walk to reach
                # the foreign slot.
                self.assertEqual(payload["foreign_calls"], 0)


    def test_a_suspended_frame_does_not_schedule_its_functions_new_code(self):
        # A frame takes its own strong reference to the code it was built
        # for, and `function.__code__` may be reassigned afterwards -- 3.11
        # only requires the free-variable counts to match.  A suspended
        # generator keeps resuming the frame it already has, so every
        # resume runs the OLD code while the function now holds a new one.
        #
        # Counting those resumes against the function's current code hands
        # that code the threshold, and its single automatic attempt, for
        # work it did not do: it is compiled from no execution history at
        # all -- the premature-compilation state the maturity policy
        # exists to avoid -- or its refusal is recorded as a verdict
        # nothing it did earned.  The counts and the attempt belong to the
        # code that actually runs.
        payload = self.run_ok(
            """
            import cinderjit

            def gen(n):
                i = 0
                while i < n:
                    yield i
                    i += 1

            def replacement(n):
                total = n - n
                i = total
                while i < n:
                    total = total + 1
                    i = i + 1
                return total

            # A shape the automatic surface refuses, so the second half
            # can tell "no attempt yet" from "attempt already spent".
            def refused(n):
                class C:
                    pass
                return C

            GEN_CODE = gen.__code__

            def resumes_only(target):
                gen.__code__ = GEN_CODE
                g = gen(@T@ * 6)
                next(g)               # the frame now holds gen's code
                gen.__code__ = target.__code__
                for _ in range(@T@ + 10):
                    next(g)           # resumes that frame, nothing else
                return [
                    e for e in observe()["events"]
                    if e["qualname"] == target.__name__
                ]

            installable = resumes_only(replacement)
            compiled_from_resumes = cinderjit.is_jit_compiled(gen)

            gen.__code__ = refused.__code__
            refusable = resumes_only(refused)

            # Only now does the replaced code actually run.
            gen.__code__ = replacement.__code__
            before = entries()
            for _ in range(@T@ * 2):
                assert gen(4) == 4
            emit(
                installable=installable,
                refusable=refusable,
                compiled_from_resumes=compiled_from_resumes,
                compiled_after_real_calls=cinderjit.is_jit_compiled(gen),
                entered=entries() - before,
                events_after=[
                    e["result"] for e in observe()["events"]
                    if e["qualname"] == "replacement"
                ],
            )
            """
        )
        # No resume of the old frame produced an event for either code
        # object the function was pointed at.
        self.assertEqual(payload["installable"], [])
        self.assertEqual(payload["refusable"], [])
        # In particular the compilable one was not compiled from work it
        # never did.
        self.assertFalse(payload["compiled_from_resumes"])
        # And once it really runs, it is scheduled normally -- the attempt
        # was still there, and the code reached it on its own frames.
        self.assertEqual(payload["events_after"], ["installed"])
        self.assertTrue(payload["compiled_after_real_calls"])
        self.assertGreater(payload["entered"], 0)


    def test_reporting_the_event_ledger_excludes_the_observer(self):
        # Building the report allocates -- a dict, two strings and a list
        # append per event -- and every one of those can collect, run a
        # finalizer, run Python, and reach the observer, which appends to
        # the very array the report is walking.  Appending can realloc()
        # it, and the element pointer the walk is holding is then freed
        # memory: under ASAN a heap-use-after-free, freed by
        # observe_events_grow and read by the reporter.
        #
        # The array is filled to exactly its capacity first, so the one
        # event the finalizer produces is the one that forces the move.
        payload = self.run_ok(
            """
            import gc

            SRC = chr(10).join(("def f(a, b):", "    return a + b", ""))

            def heat(tag):
                ns = {}
                exec(compile(SRC, "<%s>" % tag, "exec"), globals(), ns)
                globals()["k_%s" % tag] = ns["f"]
                for i in range(@T@ + 2):
                    assert ns["f"](i, 2) == i + 2

            # 1024 is the ledger's initial capacity; stop exactly on it.
            n = 0
            while len(observe()["events"]) < 1024:
                heat("fill%d" % n)
                n += 1
            before = len(observe()["events"])

            class Bomb:
                def __init__(self):
                    self.loop = self

                def __del__(self):
                    # Runs inside the report, from one of its allocations.
                    heat("bomb")

            gc.disable()
            Bomb()
            gc.set_threshold(gc.get_count()[0] + 3, 10, 10)
            gc.enable()
            snapshot = observe()
            gc.set_threshold(700, 10, 10)

            emit(
                before=before,
                reported=len(snapshot["events"]),
                malformed=sum(
                    1 for e in snapshot["events"]
                    if not isinstance(e.get("count"), int)
                    or not isinstance(e.get("qualname"), str)
                    or e.get("result") is None
                ),
                bomb_ran=any(
                    e["filename"].endswith("<bomb>")
                    for e in observe()["events"]
                ),
            )
            """,
            mode="observe",
            threshold=2,
        )
        # The ledger was full, so an append during the report would have
        # moved it.
        self.assertEqual(payload["before"], 1024)
        # Every record read back is intact.
        self.assertEqual(payload["malformed"], 0)
        # And the report is a consistent snapshot: the finalizer's own
        # frames were not counted, exactly as frames entered while the
        # observer is bookkeeping never are.
        self.assertEqual(payload["reported"], 1024)
        self.assertFalse(payload["bomb_ran"])


    def test_a_code_move_inside_the_attempt_withholds_it_and_never_burns_it(self):
        # The subject of an attempt is the code object that earned the
        # threshold, and it is fixed when the attempt begins.  Everything
        # the attempt then does runs Python -- arming the death watch
        # builds a capsule, a tuple, a C function and a weak reference;
        # preloading is documented as breaking assumptions; publication
        # allocates -- and 3.11 lets `__code__` be reassigned at any of
        # those points whenever the free-variable counts match.
        #
        # Re-reading the function at any boundary would make every later
        # check agree with the moved target, so a code object that never
        # ran a frame would be compiled and published on a threshold it
        # did not earn.  And a move must not be reported as a compile
        # failure either: that is a fact about the function, not a verdict
        # about the code, and recording it would spend the attempt for
        # good.  Both halves are swept over every landing point.
        payload = self.run_ok(
            (
                BOMB,
                NESTED,
                """
                SWAP = chr(10).join(
                    ("def swapped_in(a, b):", "    return a * b + 1", ""))

                moved, burned, ran = [], [], []
                for k in range(SWEEP):
                    make = nested_factory(k)
                    ns = {}
                    exec(compile(SWAP, "<swap%d>" % k, "exec"), globals(), ns)
                    globals()["swap%d" % k] = ns["swapped_in"]
                    target = ns["swapped_in"]

                    cinderjit.enable()
                    uncharge()
                    fn = make()
                    for i in range(@T@ - 1):
                        assert fn(i, 2) == i * 2

                    class Bomb:
                        def __init__(self):
                            self.loop = self

                        def __del__(self):
                            fn.__code__ = target.__code__

                    gc.disable()
                    Bomb()
                    gc.set_threshold(gc.get_count()[0] + k, 10, 10)
                    gc.enable()
                    fn(3, 2)
                    uncharge()
                    if fn.__code__ is not target.__code__:
                        continue          # the charge missed the attempt
                    moved.append(k)

                    # The code that never ran a frame must not have been
                    # compiled: it would run machine code if it had.
                    before = entries()
                    assert fn(2, 3) == 7
                    if entries() != before:
                        ran.append(k)

                    # And the attempt was withheld, not spent: put the
                    # code that earned it back and let it run for real.
                    fn.__code__ = make().__code__
                    for i in range(@T@ * 3):
                        assert fn(i, 2) == i * 2
                    if not cinderjit.is_jit_compiled(fn):
                        burned.append(k)
                emit(moved=moved, burned=burned, ran=ran)
                """,
            ),
        )
        # A move really did land inside the attempt.
        self.assertTrue(payload["moved"])
        # No never-executed code object was compiled ...
        self.assertEqual(payload["ran"], [])
        # ... and no code object lost its attempt to one.
        self.assertEqual(payload["burned"], [])

    def test_scheduling_is_suppressed_inside_import_and_setup_scopes(self):
        # Everything running inside an import or a setup scope runs once,
        # at a moment that says nothing about whether it is hot: the
        # import machinery's own helpers cross a low threshold simply
        # because they run before anything else does.  Suppression belongs
        # with tracing and pause -- counting continues, the attempt is not
        # spent, and the first frame after the scope closes dispatches.
        for scope in ("import", "setup"):
            with self.subTest(scope=scope):
                payload = self.run_ok(
                    (
                        HOT,
                        """
                        import cinderjit

                        enter = getattr(_cinderx, "_autojit_%s_enter")
                        leave = getattr(_cinderx, "_autojit_%s_leave")

                        before = observe()["auto_jit_disabled_codes"]
                        enter()
                        for i in range(@T@ * 3):
                            assert hot(i, 2) == i * 2
                        inside = observe()
                        inside_compiled = cinderjit.is_jit_compiled(hot)
                        leave()

                        assert hot(3, 2) == 6
                        after = observe()
                        b = entries()
                        assert hot(3, 2) == 6
                        emit(
                            # From the snapshot taken before leave(), not
                            # from the live table after it.
                            inside_events=[
                                e["result"] for e in inside["events"]
                                if e["qualname"] == "hot"
                            ],
                            inside_compiled=inside_compiled,
                            spent=inside["auto_jit_disabled_codes"] - before,
                            after_events=[
                                e["result"] for e in after["events"]
                                if e["qualname"] == "hot"
                            ],
                            compiled=cinderjit.is_jit_compiled(hot),
                            entered=entries() - b,
                        )
                        """.replace("%s", scope),
                    ),
                )
                # Nothing was scheduled and nothing was spent inside.
                self.assertEqual(payload["inside_events"], [])
                self.assertFalse(payload["inside_compiled"])
                self.assertEqual(payload["spent"], 0)
                # The attempt was still there when the scope closed.
                self.assertEqual(payload["after_events"], ["installed"])
                self.assertTrue(payload["compiled"])
                self.assertEqual(payload["entered"], 1)


    def test_the_event_ledger_does_not_hold_the_observed_program_s_strings(self):
        # The ledger records one entry per scheduling decision and is only
        # cleared at finalization, so anything it holds by reference it
        # holds for the life of the process.  Holding the code object's own
        # co_filename and co_qualname would be visible from the observed
        # program: sys.getrefcount() on either would read one higher under
        # observation than without it, and the strings would outlive the
        # code they came from.  The record keeps copies instead.
        body = """
            import sys

            SRC = chr(10).join(("def probe_fn(a, b):", "    return a + b", ""))
            ns = {}
            exec(compile(SRC, "<ledger-probe>", "exec"), ns, ns)
            fn = ns["probe_fn"]
            filename = fn.__code__.co_filename
            qualname = fn.__code__.co_qualname

            before = (sys.getrefcount(filename), sys.getrefcount(qualname))
            for i in range(@T@ * 2):
                assert fn(i, 2) == i + 2
            after = (sys.getrefcount(filename), sys.getrefcount(qualname))
            emit(
                delta=[after[0] - before[0], after[1] - before[1]],
                # The names still come back, so this is not passing by
                # recording nothing.
                recorded=[
                    (e["qualname"], e["filename"]) for e in observe()["events"]
                    if e["qualname"] == "probe_fn"
                ],
            )
            """
        off = self.run_ok(body, mode="off")
        for mode in ("observe", "shadow", "execute"):
            with self.subTest(mode=mode):
                watched = self.run_ok(body, mode=mode)
                # Observation is invisible in the refcounts of the program
                # being observed.
                self.assertEqual(watched["delta"], off["delta"])
                self.assertEqual(watched["delta"], [0, 0])
        # And the ledger really did record the names.
        recorded = self.run_ok(body, mode="execute")["recorded"]
        self.assertEqual(recorded, [["probe_fn", "<ledger-probe>"]])


    def test_suppression_covers_fresh_attachment_not_only_the_first_dispatch(self):
        # Fresh attachment is a scheduling door of its own, and it is
        # reached BEFORE the threshold path: an already-dispatched code
        # object goes through it on every later frame.  A suppression that
        # only covered the first dispatch would let an instance created
        # inside an import or setup scope attach there and start running
        # machine code, spending the code object's per-code budget at a
        # moment that says nothing about whether it is hot.
        for scope in ("import", "setup"):
            with self.subTest(scope=scope):
                payload = self.run_ok(
                    (
                        FACTORY,
                        """
                        import cinderjit

                        enter = getattr(_cinderx, "_autojit_%s_enter")
                        leave = getattr(_cinderx, "_autojit_%s_leave")

                        # Steady state reached outside any scope, so the
                        # code object is dispatched and attachable.
                        seed = factory(0)
                        for i in range(@T@ * 2):
                            assert seed(i, 2) == i * 2
                        assert cinderjit.is_jit_compiled(seed)
                        charged = observe()["fresh_attachments"]

                        enter()
                        inside = factory(0)
                        for i in range(@T@):
                            assert inside(i, 2) == i * 2
                        b = entries()
                        for i in range(10):
                            assert inside(i, 2) == i * 2
                        inside_ran = entries() - b
                        inside_attached = cinderjit.is_jit_compiled(inside)
                        inside_charged = observe()["fresh_attachments"] - charged
                        leave()

                        for i in range(@T@):
                            assert inside(i, 2) == i * 2
                        b = entries()
                        for i in range(10):
                            assert inside(i, 2) == i * 2
                        emit(
                            inside_attached=inside_attached,
                            inside_ran=inside_ran,
                            inside_charged=inside_charged,
                            after_attached=cinderjit.is_jit_compiled(inside),
                            after_ran=entries() - b,
                        )
                        """.replace("%s", scope),
                    ),
                )
                # Nothing attached, ran or was charged inside the scope.
                self.assertFalse(payload["inside_attached"])
                self.assertEqual(payload["inside_ran"], 0)
                self.assertEqual(payload["inside_charged"], 0)
                # And the attachment was withheld, not lost.
                self.assertTrue(payload["after_attached"])
                self.assertEqual(payload["after_ran"], 10)

    def test_a_pre_existing_artifact_still_obeys_the_attachment_budget(self):
        # An artifact can exist for a code object the scheduler never
        # dispatched -- force_compile() on an instance that is never
        # called is enough.  A later instance then reaches the compile
        # entry point rather than the frame-entry attachment, and the
        # generic compile path would find the existing artifact and
        # finalize onto it without consulting the per-code budget at all.
        # With a budget of zero that is the whole budget bypassed.
        body = """
            import cinderjit

            seed = factory(1)
            assert cinderjit.force_compile(seed) is True
            assert cinderjit.is_jit_compiled(seed)

            fresh = factory(2)
            for i in range(@T@ * 2):
                fresh(i, 2)
            before = entries()
            for i in range(10):
                fresh(i, 2)
            emit(
                attached=cinderjit.is_jit_compiled(fresh),
                ran=entries() - before,
            )
            """
        zero = self.run_ok((FACTORY, body), PYTHONJITFRESHATTACHBUDGET="0")
        # A budget of zero turns automatic attachment off, by every door.
        self.assertFalse(zero["attached"])
        self.assertEqual(zero["ran"], 0)
        # A budget that allows one still allows exactly this one.
        one = self.run_ok((FACTORY, body), PYTHONJITFRESHATTACHBUDGET="1")
        self.assertTrue(one["attached"])
        self.assertEqual(one["ran"], 10)


    def test_a_namespace_twin_does_not_disable_the_code_for_its_own_namespace(self):
        # A code object gets one automatic attempt and the verdict is
        # recorded on the code object, where every door reads it.  That
        # only works if the verdict is about the CODE.  A second function
        # over the same code but with foreign globals is refused because
        # the artifact belongs to the first one's namespace -- a fact
        # about that function, not about the code -- and recording it as
        # the code's verdict takes the code down with it: the instances
        # that DO share the artifact's namespace can never attach to the
        # artifact that exists for them.
        payload = self.run_ok(
            """
            import types
            import cinderjit

            SRC = chr(10).join((
                "def hot(a, b):",
                "    total = a - a",
                "    i = total",
                "    while i < b:",
                "        total = total + a",
                "        i = i + 1",
                "    return total",
            ))
            own = {"__builtins__": __builtins__}
            exec(compile(SRC, "<twin>", "exec"), own, own)
            first = own["hot"]
            code = first.__code__
            assert cinderjit.force_compile(first) is True

            # The twin: same code, foreign globals, driven to the
            # threshold so the scheduler spends the attempt on it.
            foreign = {"__builtins__": __builtins__}
            twin = types.FunctionType(code, foreign, "hot")
            foreign["hot"] = twin
            for i in range(@T@ * 2):
                assert twin(i, 2) == i * 2
            twin_verdicts = [
                e["result"] for e in observe()["events"]
                if e["qualname"] == "hot"
            ]

            # An instance that shares the artifact's namespace.
            sibling = types.FunctionType(code, own, "hot")
            own["sibling"] = sibling
            for i in range(@T@ * 2):
                assert sibling(i, 2) == i * 2
            before = entries()
            for i in range(10):
                assert sibling(i, 2) == i * 2
            emit(
                twin_verdicts=twin_verdicts,
                twin_compiled=cinderjit.is_jit_compiled(twin),
                sibling_attached=cinderjit.is_jit_compiled(sibling),
                sibling_ran=entries() - before,
            )
            """
        )
        # The twin was refused, and for the reason that is about it.
        self.assertEqual(
            payload["twin_verdicts"],
            ["REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED"],
        )
        self.assertFalse(payload["twin_compiled"])
        # And the refusal cost the code object nothing: an instance in the
        # artifact's own namespace still attaches and runs machine code.
        self.assertTrue(payload["sibling_attached"])
        self.assertEqual(payload["sibling_ran"], 10)


    def test_the_product_configuration_installs_the_import_provider(self):
        # The scheduler suppresses while the import/setup depth is above
        # zero, but something has to raise it.  On 3.12+ that is the
        # auto[:N] classifier, which also asks for the providers; 3.11
        # refuses that spelling and names the mode separately, so keying
        # the providers off the classifier left them off in exactly the
        # configuration that schedules -- execute mode with a numeric
        # threshold, which is the product configuration and the one the
        # matrix tests.
        #
        # Driven end to end, with no manual enter/leave: a real import has
        # to be the thing that raises the depth, or the suppression is a
        # mechanism with no producer.
        body = """
            import cinderx

            depths = []
            seen = []

            class Probe:
                # Imported for real, from inside the import machinery, so
                # the depth it reads is the one a real import produced.
                def find_module(self, name, path=None):
                    depths.append(_cinderx._autojit_import_depth())
                    return None

            sys.meta_path.insert(0, Probe())
            import shlex          # noqa: F401  (any unimported module)
            sys.meta_path.pop(0)

            emit(
                import_provider=cinderx._autojit_import_provider(),
                setup_provider=cinderx._autojit_setup_provider(),
                depth_during_import=max(depths) if depths else 0,
                depth_after=_cinderx._autojit_import_depth(),
            )
            """
        execute = self.run_ok(body, mode="execute", threshold=30)
        # The product configuration asks for both providers ...
        self.assertEqual(execute["import_provider"], "find_and_load")
        self.assertTrue(execute["setup_provider"])
        self.assertNotEqual(execute["setup_provider"], "off")
        # ... and a real import actually raises the depth the scheduler
        # reads, then puts it back.
        self.assertGreater(execute["depth_during_import"], 0)
        self.assertEqual(execute["depth_after"], 0)
        # With no scheduling mode configured, nothing is installed and no
        # import pays for a wrapper it does not need.
        off = self.run_ok(body, mode="off", threshold=30)
        self.assertEqual(off["import_provider"], "off")
        self.assertEqual(off["setup_provider"], "off")
        self.assertEqual(off["depth_during_import"], 0)


    # -- publication racing a control-plane decision ----------------------

    def test_disable_across_the_dispatch_never_spends_the_attempt(self):
        # A code object gets one automatic attempt.  The attempt runs
        # arbitrary Python -- outer-function registration, preloading,
        # allocation, finalizers -- so cinderjit.disable() can land inside
        # it, and the refusal that comes back then describes the process,
        # not the code object.  Recording that as the verdict would make a
        # state the next enable() undoes permanent.  One fresh code object
        # per landing point, sweeping the whole attempt.
        payload = self.run_ok(
            (
                BOMB,
                """
                SRC = chr(10).join((
                    "def hot(a, b):",
                    "    total = a - a",
                    "    i = total",
                    "    while i < b:",
                    "        total = total + a",
                    "        i = i + 1",
                    "    return total",
                ))

                deferred, lost = [], []
                seen = observe()["late_deferrals"]
                for k in range(SWEEP):
                    ns = {}
                    exec(compile(SRC, "<hot%d>" % k, "exec"), globals(), ns)
                    hot = ns["hot"]
                    # Reachable from a namespace, like any real function:
                    # this is what the outer-function walk looks for.
                    globals()["hot%d" % k] = hot
                    cinderjit.enable()
                    uncharge()
                    for i in range(@T@ - 1):
                        assert hot(i, 2) == i * 2
                    charge(k)
                    # The frame that crosses the threshold, with the
                    # collector aimed inside it.
                    assert hot(3, 2) == 6
                    uncharge()
                    cinderjit.enable()
                    now = observe()["late_deferrals"]
                    if now != seen:
                        deferred.append(k)
                    seen = now
                    # The attempt has to still be there to spend.
                    for i in range(@T@ * 2):
                        assert hot(i, 2) == i * 2
                    if not cinderjit.is_jit_compiled(hot):
                        lost.append(k)
                emit(deferred=deferred, lost=lost)
                """,
            ),
        )
        # No landing point cost a code object its attempt.
        self.assertEqual(payload["lost"], [])
        # And landing points inside the attempt exist and were reached --
        # without this the assertion above would pass vacuously.
        self.assertTrue(payload["deferred"])

    def test_disable_across_a_nested_dispatch_never_spends_the_attempt(self):
        # The same contract as the sweep above, over the shape that has an
        # outer function to register.  Scheduling a nested code object
        # arms a death watch for its outer function first, and that
        # allocates well before the compile does -- so the earliest
        # landing points for a disable() are in a stretch the compile
        # entry point has not been reached from yet.  Which protocol the
        # request follows therefore cannot be decided by asking whether
        # machine code may run right now, because by then it may not.
        payload = self.run_ok(
            (
                BOMB,
                NESTED,
                """
                deferred, lost = [], []
                seen = observe()["late_deferrals"]
                for k in range(SWEEP):
                    make = nested_factory(k)
                    cinderjit.enable()
                    uncharge()
                    fn = make()
                    for i in range(@T@ - 1):
                        assert fn(i, 2) == i * 2
                    charge(k)
                    assert fn(3, 2) == 6
                    uncharge()
                    cinderjit.enable()
                    now = observe()["late_deferrals"]
                    if now != seen:
                        deferred.append(k)
                    seen = now
                    for i in range(@T@ * 2):
                        assert fn(i, 2) == i * 2
                    if not cinderjit.is_jit_compiled(fn):
                        lost.append(k)
                emit(deferred=deferred, lost=lost)
                """,
            ),
        )
        # No landing point cost a nested code object its attempt.
        self.assertEqual(payload["lost"], [])
        # And landing points were reached, so the assertion is not vacuous.
        self.assertTrue(payload["deferred"])

    def test_disable_across_the_attach_still_charges_the_budget(self):
        # A publication that a disable() lands inside still associates the
        # function with the artifact -- the entry predicate answers "not
        # runnable right now", but enable() makes this member run machine
        # code.  Charging the budget on runnability rather than on the
        # association would let that member in for free, so the per-code
        # cap could be lifted by one for every publication a disable()
        # lands in.  With a budget of one, the second fresh instance must
        # be refused however the first one got in.
        payload = self.run_ok(
            (
                NESTED,
                BOMB,
                """
                landed, over = [], []
                for k in range(SWEEP):
                    make = nested_factory(k)
                    cinderjit.enable()
                    uncharge()
                    seed = make()
                    for i in range(@T@ * 2):
                        assert seed(i, 2) == i * 2
                    if not cinderjit.is_jit_compiled(seed):
                        continue
                    first = make()
                    charge(k)
                    assert first(3, 2) == 6
                    uncharge()
                    if cinderjit.is_enabled():
                        continue
                    landed.append(k)
                    cinderjit.enable()
                    before = entries()
                    for i in range(20):
                        assert first(i, 2) == i * 2
                    first_runs = entries() - before
                    # The one attachment the budget allows is spent; a
                    # second instance must stay interpreted.
                    second = make()
                    for i in range(@T@ * 2):
                        assert second(i, 2) == i * 2
                    before = entries()
                    for i in range(20):
                        assert second(i, 2) == i * 2
                    second_runs = entries() - before
                    if first_runs and second_runs:
                        over.append((k, first_runs, second_runs))
                emit(landed=landed, over=over)
                """,
            ),
            PYTHONJITFRESHATTACHBUDGET="1",
        )
        # The budget was never exceeded ...
        self.assertEqual(payload["over"], [])
        # ... and a disable() really did land inside a publication, so the
        # assertion above is about the window it is named for.
        self.assertTrue(payload["landed"])


    def test_disable_across_the_attach_keeps_machine_code_shut(self):
        # Fresh attachment publishes an entry point for a new function
        # object over already-compiled code, and publishing allocates.  A
        # disable() can therefore land between the sweep that deopts the
        # registry and the moment this entry point goes in, so the sweep
        # never sees it.  Entering machine code behind a disable() is the
        # failure: the entry predicate has to answer on the state now, not
        # on the state the publication started in.
        payload = self.run_ok(
            (
                FACTORY,
                BOMB,
                """
                seed = factory(0)
                for i in range(@T@ * 2):
                    assert seed(i, 2) == i * 2
                assert cinderjit.is_jit_compiled(seed)

                landed, entered = [], []
                attachments = observe()["fresh_attachments"]
                for k in range(SWEEP):
                    cinderjit.enable()
                    uncharge()
                    fresh = factory(0)
                    charge(k)
                    # First call over compiled code: the frame that
                    # attaches, with the collector aimed inside it.
                    assert fresh(3, 2) == 6
                    uncharge()
                    if cinderjit.is_enabled():
                        continue
                    landed.append(k)
                    before = entries()
                    for i in range(20):
                        assert fresh(i, 2) == i * 2
                    if entries() != before:
                        entered.append((k, entries() - before))
                cinderjit.enable()
                before = entries()
                fresh = factory(0)
                for i in range(20):
                    assert fresh(i, 2) == i * 2
                emit(
                    landed=landed,
                    entered=entered,
                    attached=observe()["fresh_attachments"] - attachments,
                    resumed=entries() - before,
                )
                """,
            ),
        )
        # The disable really landed inside some publication, and fresh
        # attachment really ran -- neither assertion below is vacuous.
        self.assertTrue(payload["landed"])
        self.assertGreaterEqual(payload["attached"], 1)
        # No frame entered machine code while the JIT was unusable.
        self.assertEqual(payload["entered"], [])
        # And attachment still works once it is usable again.
        self.assertGreater(payload["resumed"], 0)


if __name__ == "__main__":
    unittest.main()
