# Copyright (c) Meta Platforms, Inc. and affiliates.
"""MR-04 canary execution gate (CPython 3.11).

The default build keeps machine code structurally unreachable; the canary
mode flips exactly that, in a child process, and this gate holds both
sides: the child genuinely executes machine code with a closed ledger and
the attribute-cache default stays off, while the parent (default mode)
counters remain zero.
"""

import json
import os
import subprocess
import sys
import textwrap
import unittest

CHILD = textwrap.dedent(
    """
    import json
    import _cinderx, cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    assert cinderjit.is_attr_caches_enabled() is False, (
        "3.11 attribute caches must default off until MR-09")

    def hot(a, b, one):
        total = a - a
        i = total
        while i < b:
            total = total + a
            i = i + one
        return total

    expected = [hot(i, 7, 1) for i in range(32)]
    assert cinderjit.force_compile(hot) is True
    assert cinderjit.is_jit_compiled(hot)
    for i in range(32):
        assert hot(i, 7, 1) == expected[i], i

    stats = _cinderx._get_trigger_stats()
    print(json.dumps(stats))
    """
)


class CanaryExecute311Test(unittest.TestCase):
    def test_canary_child_executes_and_default_stays_zero(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        proc = subprocess.run(
            [sys.executable, "-c", CHILD],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        stats = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertGreaterEqual(stats["compiled_function_creations"], 1)
        self.assertGreaterEqual(stats["executable_alloc_calls"], 1)
        self.assertGreater(stats["executable_alloc_bytes"], 0)
        self.assertGreaterEqual(stats["machine_code_entries"], 32)
        self.assertEqual(stats["shadow_compile_success"], 0)

    def test_canary_rejects_functions_outside_the_execute_surface(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def uses_call(a):
                return len(str(a))

            try:
                cinderjit.force_compile(uses_call)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("execute surface failed to refuse CALL")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])


    def test_escaped_frame_stays_accessible_after_return(self):
        # MR-04 acceptance: a frame that escaped via an exception traceback
        # must stay safely accessible after the machine-code call returned
        # and after collection (the materialized _PyInterpreterFrame's
        # ownership handed over correctly).
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def raiser(x):
                none = None
                return x + none

            assert cinderjit.force_compile(raiser) is True
            frames = []
            for _ in range(3):
                try:
                    raiser(7)
                except TypeError as exc:
                    tb = exc.__traceback__
                    while tb.tb_next is not None:
                        tb = tb.tb_next
                    frames.append(tb.tb_frame)
            gc.collect()
            for frame in frames:
                assert frame.f_code.co_name == "raiser"
                assert frame.f_locals["x"] == 7
                assert frame.f_lineno > 0
            del frames
            gc.collect()
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_execute_surface_refuses_unaudited_argument_shapes(self):
        # Argument binding beyond exact positional arguments -- keyword-only
        # parameters, defaults, and the variadic collectors -- carries error
        # and fallback contracts that MR-06 owns.  A body made only of
        # whitelisted opcodes must not be enough to let those signatures
        # execute machine code.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def kwonly(*, x):
                return x
            def varargs(*args):
                return args
            def varkw(**kwargs):
                return kwargs
            def mixed(a, *, b=2):
                return a
            def defaulted(a, b=3):
                return a

            for fn in (kwonly, varargs, varkw, mixed, defaulted):
                try:
                    cinderjit.force_compile(fn)
                except RuntimeError as exc:
                    assert "CANNOT_SPECIALIZE" in str(exc), (fn, exc)
                else:
                    raise SystemExit(
                        f"execute surface accepted {fn.__name__}")
                assert not cinderjit.is_jit_compiled(fn), fn

            # The same body with plain positional parameters still compiles,
            # so the refusal is about the signature and not the body.
            def positional(a, b, one):
                return a
            assert cinderjit.force_compile(positional) is True
            assert cinderjit.is_jit_compiled(positional)
            print("argument shapes refused")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("argument shapes refused", proc.stdout)

    def test_compiled_artifact_is_never_shared_between_functions(self):
        # CPython 3.11 has no function-destroy notification, so a second
        # owner of one compiled artifact would keep it alive past the first
        # owner's death and leave that dead function as a borrowed pointer
        # in the registry -- reading it then segfaults.  Until the MR-05
        # lifecycle lands, a function whose (code, globals, builtins) is
        # already compiled for someone else is refused.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import types
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def twin(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            other = types.FunctionType(
                twin.__code__, twin.__globals__, "other")
            assert cinderjit.force_compile(twin) is True
            try:
                cinderjit.force_compile(other)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("a second owner was allowed to share")
            assert not cinderjit.is_jit_compiled(other)
            assert other(3, 5, 1) == twin(3, 5, 1)

            # The twin dies; the registry must stay readable and the
            # surviving function must keep running its machine code.
            del other
            gc.collect()
            for func in cinderjit.get_compiled_functions():
                assert func.__qualname__, func
            assert twin(3, 5, 1) == 15
            print("artifact ownership stayed exclusive")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("artifact ownership stayed exclusive", proc.stdout)

    def test_specialized_bytecode_does_not_become_a_speculative_guard(self):
        # Warm compilation reads the interpreter's quickened forms.  Turning
        # those into type guards is MR-07 work, so canary compiles the
        # unspecialized forms: a warm function must keep answering
        # correctly when the argument type changes, with no deopt.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import dis
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def arith(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                arith(3, 5, 1)
            specialized = [
                instr.opname
                for instr in dis.get_instructions(arith, adaptive=True)
                if instr.opname.endswith("_INT")
            ]
            assert specialized, "the interpreter never specialized the target"

            assert cinderjit.force_compile(arith) is True
            cinderjit.get_and_clear_runtime_stats()
            assert arith(3, 5, 1) == 15
            assert arith(3.0, 5.0, 1.0) == 15.0
            assert cinderjit.is_jit_compiled(arith)
            deopts = cinderjit.get_and_clear_runtime_stats().get("deopt", [])
            assert not deopts, deopts
            print("no speculative guard on the execute surface")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("no speculative guard", proc.stdout)

    def test_canary_control_plane_is_restricted(self):
        # The full cinderjit method table is a control surface for
        # capabilities MR-04 does not have: the batch and lazy paths install
        # machine code without passing the execute surface, force_uncompile
        # belongs to MR-05, and the specialization and guard setters can
        # re-open exactly the speculation this milestone excludes.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            # Capabilities later milestones own.
            withheld = [
                "lazy_compile", "precompile_all", "compile_all",
                "force_uncompile", "enable_specialized_opcodes",
                "disable_specialized_opcodes",
                "enable_emit_type_annotation_guards",
                "clear_runtime_stats",
            ]
            present = [name for name in withheld if hasattr(cinderjit, name)]
            assert not present, present

            # What this milestone's evidence needs, plus every API that
            # stops the JIT rather than extending it: withholding those
            # left the wrapper with stubs that silently did nothing.
            needed = [
                "force_compile", "is_jit_compiled", "is_attr_caches_enabled",
                "get_compiled_functions", "get_and_clear_runtime_stats",
                "is_enabled", "jit_suppress", "jit_unsuppress",
                "disable", "enable", "_get_resident_compiled_functions",
            ]
            missing = [name for name in needed if not hasattr(cinderjit, name)]
            assert not missing, missing
            print("control plane restricted")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("control plane restricted", proc.stdout)

    def test_entry_rechecks_code_identity_and_call_form(self):
        # 3.11 has no function watchers, so nothing reports a __code__ or
        # defaults change after compilation, and the compiled body assumes
        # the exact positional call form the surface accepted.  The entry
        # re-checks both on every call and hands anything else back to the
        # interpreter.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def target(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(target) is True
            before = entries()
            assert target(3, 5, 1) == 15
            assert entries() - before == 1, "positional call must run compiled"

            # Keyword form: the compiled body never bound keywords.
            before = entries()
            assert target(a=3, b=5, one=1) == 15
            assert entries() - before == 0, "keyword call must not run compiled"

            # Defaults appearing after compilation.
            before = entries()
            target.__defaults__ = (1,)
            assert target(3, 5) == 15
            assert entries() - before == 0, "defaults must not run compiled"
            target.__defaults__ = None

            # A different code object behind the same function.
            def replacement(a, b, one):
                return "replaced"

            target.__code__ = replacement.__code__
            before = entries()
            assert target(3, 5, 1) == "replaced", "stale machine code ran"
            assert entries() - before == 0
            print("entry rechecked identity and call form")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("entry rechecked identity", proc.stdout)

    def test_artifacts_carry_no_speculative_guard(self):
        # Guards do not only come from quickened opcodes: Simplify installs
        # its own for `x ** 2`, for compact-long comparisons and for float
        # division, each with a deopt behind it.  The executing mode
        # therefore compiles without that pass, and the optimized artifact
        # is scanned so anything that still carries a guard is refused
        # rather than shipped.  What this test holds is the observable
        # consequence: shapes that would have been guarded now execute for
        # any argument type, with no deopt at all.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def square(x):
                return x ** 2

            def loop(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(square) is True
            assert cinderjit.force_compile(loop) is True
            assert cinderjit.is_jit_compiled(square)
            assert cinderjit.is_jit_compiled(loop)

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            cinderjit.get_and_clear_runtime_stats()
            before = entries()
            # Both argument types, on both shapes: a speculative guard would
            # deopt on whichever type it did not bake in.
            assert square(3.0) == 9.0
            assert square(3) == 9
            assert loop(3, 5, 1) == 15
            assert loop(3.0, 5.0, 1.0) == 15.0
            assert entries() - before == 4, entries() - before
            deopts = cinderjit.get_and_clear_runtime_stats().get("deopt", [])
            assert not deopts, deopts
            print("artifacts carry no speculative guard")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("artifacts carry no speculative guard", proc.stdout)

    def test_entry_survives_the_artifact_being_dropped_mid_call(self):
        # The body runs arbitrary Python through its operators, and that
        # code can drop the artifact's last reference -- clearing the
        # function's __dict__ is a documented way to uncompile -- which
        # would free the code buffer currently executing.  The entry pins
        # the artifact for the duration of the call.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            holder = None

            class Killer:
                def __add__(self, other):
                    # Drop the artifact's only strong reference while its
                    # machine code is on the stack.
                    holder.__dict__.clear()
                    gc.collect()
                    return 42

            def victim(a, b):
                return a + b

            holder = victim
            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(victim)
            assert victim(Killer(), object()) == 42
            gc.collect()
            # The function keeps working afterwards, interpreted now that
            # its artifact is gone.
            assert victim(1, 2) == 3
            print("entry survived the artifact being dropped")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("entry survived", proc.stdout)

    def test_entry_refuses_an_artifact_owned_by_another_function(self):
        # Assigning a compiled function's __code__ onto another function
        # would otherwise run an artifact anchored only by the donor: the
        # code identity matches, the globals match, and nothing else looked
        # at ownership.  If the donor then died, the borrower would be
        # executing freed machine code.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            # Same module globals on purpose: that is the case where every
            # other check the entry makes would pass.
            def borrower(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            def donor(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(borrower) is True
            assert cinderjit.force_compile(donor) is True

            borrower.__code__ = donor.__code__
            before = entries()
            assert borrower(3, 5, 1) == 15
            assert entries() - before == 0, "ran an artifact it does not own"
            assert not cinderjit.is_jit_compiled(borrower)
            # The donor keeps running its own artifact.
            before = entries()
            assert donor(3, 5, 1) == 15
            assert entries() - before == 1
            print("borrowed artifact refused")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("borrowed artifact refused", proc.stdout)

    def test_jit_wrapper_reports_the_truth_on_a_restricted_build(self):
        # cinderx.jit imports the whole control plane in one statement, so a
        # capability-gated build made every name fall back to a no-op stub:
        # the wrapper reported the JIT as absent while machine code was
        # demonstrably executing, and tests gated on is_enabled() skipped.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert jit.is_enabled(), "wrapper reports the JIT as absent"
            assert jit.force_compile(hot) is True
            assert jit.is_jit_compiled(hot) is True
            assert any(f is hot for f in jit.get_compiled_functions())
            assert hot(3, 5, 1) == 15
            print("wrapper reports the truth")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("wrapper reports the truth", proc.stdout)

    def test_compiled_state_matches_what_the_entry_will_do(self):
        # "Is this compiled?" and "will this call run machine code?" have to
        # be the same question.  Growing defaults after compilation sends
        # every call to the interpreter, so reporting the function as
        # compiled -- and counting it as installed -- would describe a state
        # the runtime is not in.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(hot) is True
            before = entries()
            assert hot(3, 5, 1) == 15
            assert entries() - before == 1
            assert cinderjit.is_jit_compiled(hot)
            assert any(f is hot for f in cinderjit.get_compiled_functions())

            for mutate, restore in (
                (lambda: setattr(hot, "__defaults__", (1,)),
                 lambda: setattr(hot, "__defaults__", None)),
                (lambda: setattr(hot, "__kwdefaults__", {"one": 1}),
                 lambda: setattr(hot, "__kwdefaults__", None)),
            ):
                mutate()
                before = entries()
                assert hot(3, 5, 1) == 15
                delta = entries() - before
                compiled = cinderjit.is_jit_compiled(hot)
                listed = any(f is hot
                             for f in cinderjit.get_compiled_functions())
                assert delta == 0, delta
                assert not compiled, "reported compiled but ran interpreted"
                assert not listed, "listed as installed but ran interpreted"
                restore()
                # Restoring the state restores the answer, both ways.
                before = entries()
                assert hot(3, 5, 1) == 15
                assert entries() - before == 1
                assert cinderjit.is_jit_compiled(hot)
            print("state matches the entry")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("state matches the entry", proc.stdout)

    def test_pinned_artifact_keeps_its_function_alive(self):
        # The artifact is an ordinary object in the function's __dict__, so
        # Python can hold it on its own.  With borrowed references the
        # function could then die while the registry still pointed at it,
        # and reading the registry dereferenced freed memory.  On 3.11 the
        # artifact owns its function instead; the cycle is collectable, so
        # dropping the pin still releases both.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace["victim"]
            del namespace
            assert cinderjit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15

            pin = victim.__dict__["__cinderx_compiled_func__"]
            alive = weakref.ref(victim)
            del victim
            gc.collect()
            # Churn the allocator so a freed function object would be reused.
            junk = [bytearray(400) for _ in range(5000)]

            assert alive() is not None, "the artifact did not keep it alive"
            listed = cinderjit.get_compiled_functions()
            assert len(listed) == 1, listed
            assert listed[0].__qualname__ == "victim"
            assert listed[0](3, 5, 1) == 15
            del listed, junk

            del pin
            gc.collect()
            assert cinderjit.get_compiled_functions() == []
            assert alive() is None, "the cycle did not collect"
            print("pinned artifact kept its function alive")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("pinned artifact kept its function alive", proc.stdout)

    def test_suppression_and_pause_actually_stop_the_jit(self):
        # A milestone may withhold what it cannot do; it may not withhold
        # what stops it doing something.  While the canary module hid
        # jit_suppress, disable and enable, the wrapper kept no-op stubs
        # for them: @jit_suppress returned the function unchanged without
        # setting the suppress flag, so a function marked "do not compile"
        # compiled and executed anyway, and pause() disabled nothing.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            @jit.jit_suppress
            def suppressed(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            try:
                cinderjit.force_compile(suppressed)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("suppression did not suppress")
            assert not cinderjit.is_jit_compiled(suppressed)
            before = entries()
            assert suppressed(3, 5, 1) == 15
            assert entries() - before == 0

            def paused(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(paused) is True
            with jit.pause(deopt_all=True):
                assert not cinderjit.is_jit_compiled(paused)
                before = entries()
                assert paused(3, 5, 1) == 15
                assert entries() - before == 0, "pause did not pause"

            # And un-pausing must put it back: a pause that cannot be
            # undone is a one-way door, which is how re-arming after the
            # re-attach loop rather than before it went unnoticed.
            assert cinderjit.is_jit_compiled(paused), "enable did not restore"
            before = entries()
            assert paused(3, 5, 1) == 15
            assert entries() - before == 1, "machine code did not come back"

            # A control API the build genuinely withholds must say so
            # rather than report success.
            try:
                jit.precompile_all()
            except RuntimeError:
                pass
            else:
                raise SystemExit("a withheld API returned success")
            print("suppression and pause hold")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("suppression and pause hold", proc.stdout)

    def test_function_dying_while_paused_survives_re_enable(self):
        # disable(deopt_all=True) parks every compiled function in the
        # deopted set, which re-enabling walks again.  With borrowed
        # references and no function watcher to clear them, a function that
        # died while paused was dereferenced on the way back.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace["victim"]
            del namespace
            assert jit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15
            alive = weakref.ref(victim)

            with jit.pause(deopt_all=True):
                del victim
                gc.collect()
                junk = [bytearray(400) for _ in range(5000)]
                # The parked function must stay valid for as long as the
                # runtime can walk it again.
                assert alive() is not None
                del junk
            # Re-enabling walks the parked set; nothing here may dangle.
            gc.collect()
            assert alive() is None or alive().__qualname__ == "victim"
            print("survived re-enable")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("survived re-enable", proc.stdout)

    def test_resident_count_is_physical_not_logical(self):
        # The resident metric answers "is a code buffer still alive", so it
        # may not depend on whether the JIT happens to be paused, and it may
        # not drop a deopted-but-resident artifact.  Reporting zero while
        # machine code is still installed is the false negative it exists
        # to prevent.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(hot) is True
            running = cinderjit._get_resident_compiled_functions()
            assert isinstance(running, int), type(running)
            assert running >= 1, running

            cinderjit.disable()
            paused = cinderjit._get_resident_compiled_functions()
            assert isinstance(paused, int), type(paused)
            assert paused >= running, (paused, running)
            cinderjit.enable()
            print("resident count is physical")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("resident count is physical", proc.stdout)

    def test_cold_and_warm_entries_refuse_the_wrong_timing(self):
        # The plan names cold and warm compilation as separate entry
        # points because they compile different inputs.  Leaving the
        # distinction to whether the caller happened to warm the function
        # first makes it an accident; each entry refuses the other timing.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit

            def make(name):
                namespace = {}
                exec(
                    "def " + name + "(a, b, one):\\n"
                    "    total = a - a\\n"
                    "    i = total\\n"
                    "    while i < b:\\n"
                    "        total = total + a\\n"
                    "        i = i + one\\n"
                    "    return total\\n",
                    namespace,
                )
                return namespace[name]

            cold = make("cold")
            assert jit.force_compile_cold(cold) is True
            assert jit.is_jit_compiled(cold)

            warm = make("warm")
            try:
                jit.force_compile_warm(warm)
            except RuntimeError as exc:
                assert "not been specialized" in str(exc), exc
            else:
                raise SystemExit("warm entry accepted an unquickened function")
            for _ in range(200):
                warm(3, 5, 1)
            assert jit.force_compile_warm(warm) is True
            assert jit.is_jit_compiled(warm)

            already = make("already")
            for _ in range(200):
                already(3, 5, 1)
            try:
                jit.force_compile_cold(already)
            except RuntimeError as exc:
                assert "already run" in str(exc), exc
            else:
                raise SystemExit("cold entry accepted a quickened function")
            print("cold and warm entries hold their timings")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("cold and warm entries hold", proc.stdout)

    def test_ten_thousand_calls_leave_no_reference_drift(self):
        # MR-04 acceptance: after 10,000 machine-code calls the function, its
        # code object and the arguments must show zero refcount drift, on the
        # normal-return path and on the exception path alike.  Retained
        # entry frames show up here first: a frame that is never cleared and
        # popped keeps its arguments alive, so argument drift is the
        # sensitive detector for frame-chain residue.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        # No call threshold: organic scheduling and the ROI deopt backoff both
        # key off compile_after_n_calls, so leaving it unset keeps the two
        # functions below under force-compile control for the whole run and
        # lets the exception path stay in machine code past the backoff
        # budget (whose own behaviour is covered separately).
        env.pop("PYTHONJITAUTO", None)
        probe = textwrap.dedent(
            """
            import gc
            import json
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            CALLS = 10000

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            def raiser(x, none):
                return x + none

            assert cinderjit.force_compile(hot) is True
            assert cinderjit.force_compile(raiser) is True

            class Arg(int):
                pass

            arg = Arg(3)
            payload = object()

            def burn_normal(n):
                for _ in range(n):
                    hot(arg, 5, 1)

            def burn_raising(n):
                for _ in range(n):
                    try:
                        raiser(payload, None)
                    except TypeError:
                        pass

            # Warm both paths first: the first calls legitimately publish
            # caches and stubs, so the baseline is taken after that settles.
            burn_normal(16)
            burn_raising(16)
            gc.collect()

            before = {
                "hot": sys.getrefcount(hot),
                "hot_code": sys.getrefcount(hot.__code__),
                "raiser": sys.getrefcount(raiser),
                "raiser_code": sys.getrefcount(raiser.__code__),
                "arg": sys.getrefcount(arg),
                "payload": sys.getrefcount(payload),
            }
            entries_before = _cinderx._get_trigger_stats()[
                "machine_code_entries"]

            burn_normal(CALLS)
            burn_raising(CALLS)
            gc.collect()

            after = {
                "hot": sys.getrefcount(hot),
                "hot_code": sys.getrefcount(hot.__code__),
                "raiser": sys.getrefcount(raiser),
                "raiser_code": sys.getrefcount(raiser.__code__),
                "arg": sys.getrefcount(arg),
                "payload": sys.getrefcount(payload),
            }
            drift = {k: after[k] - before[k] for k in before}
            assert all(v == 0 for v in drift.values()), drift

            # Every one of those calls must have entered machine code and
            # linked its own frame; a stuck counter would make the drift
            # assertion vacuous.
            entries = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert entries - entries_before == 2 * CALLS, (
                entries - entries_before, 2 * CALLS)
            assert cinderjit.is_jit_compiled(hot)
            assert cinderjit.is_jit_compiled(raiser)
            print(json.dumps({"drift": drift, "entries": entries}))
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertEqual(set(report["drift"].values()), {0}, report)

    def test_repeated_deopts_demote_the_function_safely(self):
        # Machine code that keeps deopting is withdrawn by the shared ROI
        # backoff policy once its deopt budget is spent.  MR-04 is the first
        # point at which that transition is reachable on 3.11 -- nothing
        # executed before it, so nothing could deopt -- and the transition
        # unpatches an entry point that calls are still arriving at.  Results
        # must stay correct across it, the machine-code counter must stop
        # growing once the function is withdrawn, and nothing may drift.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        # The backoff only arms when a call threshold exists; a huge one keeps
        # organic compilation out of the way while leaving the policy live.
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import json
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def raiser(x, none):
                return x + none

            assert cinderjit.force_compile(raiser) is True
            payload = object()

            def burn(n):
                raised = 0
                for _ in range(n):
                    try:
                        raiser(payload, None)
                    except TypeError:
                        raised += 1
                return raised

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            # Spend the budget.  Every call must still raise TypeError, both
            # while compiled and after the withdrawal.
            assert burn(16) == 16
            assert cinderjit.is_jit_compiled(raiser)
            armed = entries()
            assert armed == 16, armed

            assert burn(2048) == 2048
            assert not cinderjit.is_jit_compiled(raiser), (
                "the deopt budget never withdrew the machine code")
            withdrawn = entries()

            gc.collect()
            before = (
                sys.getrefcount(raiser),
                sys.getrefcount(raiser.__code__),
                sys.getrefcount(payload),
            )
            assert burn(2048) == 2048
            gc.collect()
            after = (
                sys.getrefcount(raiser),
                sys.getrefcount(raiser.__code__),
                sys.getrefcount(payload),
            )
            assert before == after, (before, after)
            # Withdrawn means withdrawn: no further machine-code entries.
            assert entries() == withdrawn, (entries(), withdrawn)
            print(json.dumps({"armed": armed, "withdrawn": withdrawn}))
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        # The withdrawal happens at the configured budget, well before the
        # 2048 calls that follow: the counter must stop far below them.
        self.assertLess(report["withdrawn"], 2048, report)

    def test_code_extra_write_does_not_arm_foreign_slots(self):
        # Regression: CPython's _PyCode_SetExtra sizes a fresh co_extra to the
        # interpreter's total registered index count, and code_dealloc then
        # invokes EVERY registered freefunc below that size -- including for
        # slots holding NULL.  Attaching our counter to a code object must
        # therefore not enlarge its co_extra past our own index, or every code
        # object the runtime touches starts calling third-party freefuncs it
        # never stored anything for.  Lib/test/test_code.py registers such a
        # freefunc as a ctypes closure, which dies during shutdown; before the
        # fix that combination segfaulted at finalization.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1"
        # On 3.11 the only writer of a fresh code object's co_extra is
        # compilation, and compiled code is otherwise pinned for the life of
        # the process by the twin-dedup cache -- which would leave the
        # assertion below with nothing to observe.  Turning dedup off is what
        # lets the subject both be written and then die.
        env["CINDERX_AUTOJIT_CODE_DEDUP"] = "0"
        probe = textwrap.dedent(
            """
            import ctypes
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            # Claim two indices after cinderx, so both are above ours.
            FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
            request = ctypes.pythonapi._PyEval_RequestCodeExtraIndex
            request.argtypes = (ctypes.c_void_p,)
            request.restype = ctypes.c_ssize_t

            freed = []
            low_cb = FREEFUNC(lambda ptr: freed.append("low"))
            low = request(ctypes.cast(low_cb, ctypes.c_void_p))
            high_cb = FREEFUNC(lambda ptr: freed.append("high"))
            high = request(ctypes.cast(high_cb, ctypes.c_void_p))
            assert 0 < low < high, (low, high)
            index = low

            # A code object the runtime attaches its extra data to, then
            # drops.  The body stays inside the execute surface so that it
            # actually compiles: compilation is what writes co_extra.
            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace["victim"]

            # Liveness probe: a sentinel smuggled into the code object's
            # constants dies exactly when the code object does, so the
            # assertion cannot pass merely because nothing was deallocated.
            class Sentinel:
                pass

            sentinel = Sentinel()
            victim.__code__ = victim.__code__.replace(
                co_consts=victim.__code__.co_consts + (sentinel,))
            code_alive = weakref.ref(sentinel)
            del sentinel

            for _ in range(64):
                victim(3, 5, 1)
            # If a surface change ever stopped this from compiling, nothing
            # would write co_extra and the assertion below would pass for
            # the wrong reason.
            assert cinderjit.is_jit_compiled(victim), (
                "the probe subject did not compile, so nothing wrote "
                "co_extra and the check below would be vacuous")
            del namespace["victim"]
            del victim
            gc.collect()
            assert code_alive() is None, (
                "the victim code object outlived the probe; the check below "
                "would be vacuous")

            assert freed == [], (
                "our extra-data write enlarged co_extra into foreign slots; "
                f"freefuncs above index {index} ran: {freed}")

            # Control, through the stock path: writing the LOW foreign index
            # with CPython's own setter sizes co_extra to the interpreter-wide
            # index count, so dying also invokes the HIGH freefunc for a slot
            # nothing was ever stored in.  This is the upstream behaviour the
            # capped writer exists to avoid -- and it proves the probe above
            # can actually observe a foreign freefunc firing.
            set_extra = ctypes.pythonapi._PyCode_SetExtra
            set_extra.argtypes = (
                ctypes.py_object, ctypes.c_ssize_t, ctypes.c_void_p)
            set_extra.restype = ctypes.c_int
            namespace = {}
            exec("def control():\\n    return 1\\n", namespace)
            control = namespace["control"]
            assert set_extra(control.__code__, low, 0x1234) == 0
            del namespace["control"]
            del control
            gc.collect()
            assert freed == ["low", "high"], freed

            print("foreign extra slots stayed disarmed")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("foreign extra slots stayed disarmed", proc.stdout)

    def test_auto_threshold_alone_does_not_execute(self):
        # MR-04 acceptance: the product auto-JIT stays unavailable -- a
        # bare PYTHONJITAUTO without the explicit canary mode must leave
        # every counter at zero and provide no cinderjit module.
        env = dict(os.environ)
        env.pop("CINDERX_JIT_MODE", None)
        env["PYTHONJITAUTO"] = "5"
        probe = textwrap.dedent(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            try:
                import cinderjit
            except ImportError:
                pass
            else:
                raise SystemExit("cinderjit must not exist without canary")

            def hot(a, b, one):
                i = a - a
                while i < b:
                    i = i + one
                return i

            for j in range(64):
                hot(j, 5, 1)
            stats = _cinderx._get_trigger_stats()
            assert all(v == 0 for v in stats.values()), stats
            print("auto-alone stays gated")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])


if __name__ == "__main__":
    unittest.main()
