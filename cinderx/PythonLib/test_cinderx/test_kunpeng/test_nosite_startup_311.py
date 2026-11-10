# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Real -S startup: native readiness, import scopes and lazy metadata."""

import importlib.util
import os
import platform
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:3] == (3, 11, 6), "requires CPython 3.11.6")
class NoSiteStartup311Tests(unittest.TestCase):
    def run_child(self, source, *, extra_env=None, expected=0, before=""):
        spec = importlib.util.find_spec("_cinderx")
        if spec is None or spec.origin is None:
            self.skipTest("requires an installed CinderX native extension")
        package_dir = str(Path(spec.origin).parent)
        env = {
            name: value
            for name, value in os.environ.items()
            if not name.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))
            and name != "PYTHONPATH"
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="1",
            CINDERX_EVAL_MODE="cinder",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="4",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        if extra_env:
            env.update(extra_env)
        code = (
            "import sys\nsys.path.insert(0, " + repr(package_dir) + ")\n"
            + textwrap.dedent(before)
            + "\nimport _cinderx_auto\n"
            + textwrap.dedent(source)
        )
        result = subprocess.run(
            [sys.executable, "-S", "-c", code],
            env=env,
            capture_output=True,
            text=True,
            timeout=45,
        )
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_generator_pool_exhaustion_recycling_and_deopt(self):
        self.run_child("""
            import _cinderx, cinderjit, gc

            def numbers():
                yield 11
                yield 23

            assert cinderjit.force_compile(numbers)
            # Exceed the 2048-slot arena, then mix untouched, recycled and
            # ordinary GC allocations. Repeat to catch stale free-list links.
            for cycle in range(3):
                generators = [numbers() for _ in range(2065)]
                assert all(next(g) == 11 for g in generators)
                for i in range(0, len(generators), 2):
                    generators[i].close()
                    generators[i] = None
                gc.collect()
                replacements = [numbers() for _ in range(1100)]
                assert all(next(g) == 11 for g in replacements)
                survivors = [g for g in generators if g is not None]
                # Materializing frames exercises arena-aware deoptimization.
                assert all(g.gi_frame is not None for g in survivors)
                assert all(next(g) == 23 for g in survivors)
                for g in survivors + replacements:
                    g.close()
                del generators, replacements, survivors, g
                gc.collect()
            assert cinderjit.is_enabled()
        """, extra_env={"PYTHONJITGENERATOR": "1"})

    @unittest.skipUnless(platform.libc_ver()[0] == "glibc", "glibc fork hook")
    def test_fork_memory_reclaim_runs_only_after_new_compilation(self):
        self.run_child("""
            import cinderjit
            # Consume any initialization-era compilation once.
            cinderjit._before_fork_parent()
            assert cinderjit._before_fork_parent() is False
            def first(x): return x + 7
            assert cinderjit.force_compile(first)
            assert cinderjit._before_fork_parent() is True
            assert cinderjit._before_fork_parent() is False
            assert cinderjit.is_jit_compiled(first)
            assert first(5) == 12
            def second(x): return x * 3
            assert cinderjit.force_compile(second)
            assert cinderjit._before_fork_parent() is True
            assert cinderjit._before_fork_parent() is False
            assert second(5) == 15
            assert cinderjit.is_enabled()
        """, extra_env={"PYTHONJITAUTO": "1000000"})

    @unittest.skipUnless(platform.libc_ver()[0] == "glibc", "glibc fork hook")
    def test_fork_reclaim_preserves_parent_and_child_jit_execution(self):
        self.run_child("""
            import os, cinderjit, _cinderx
            assert any(
                kw.get('before') is cinderjit._before_fork_parent
                and kw.get('after_in_child') is cinderjit.after_fork_child
                for kw in registrations
            )
            cinderjit._before_fork_parent()
            def live(x): return x + 7
            assert cinderjit.force_compile(live)
            rd, wr = os.pipe()
            pid = os.fork()
            if pid == 0:
                os.close(rd)
                try:
                    assert _cinderx.is_frame_evaluator_installed()
                    assert cinderjit.is_enabled()
                    assert live(5) == 12
                    def child_only(x): return x * 3
                    assert cinderjit.force_compile(child_only)
                    assert child_only(5) == 15
                    assert cinderjit.is_jit_compiled(child_only)
                    os.write(wr, b'ok')
                except BaseException:
                    os._exit(1)
                os._exit(0)
            os.close(wr)
            message = os.read(rd, 2)
            os.close(rd)
            _, status = os.waitpid(pid, 0)
            assert os.waitstatus_to_exitcode(status) == 0, status
            assert message == b'ok', message
            # The automatically registered before callback consumed this
            # compilation; another direct call must not rescan the allocator.
            assert cinderjit._before_fork_parent() is False
            assert cinderjit.is_jit_compiled(live)
            assert live(5) == 12
        """, extra_env={"PYTHONJITAUTO": "1000000"}, before="""
            import posix
            registrations = []
            original_register = posix.register_at_fork
            def record_registration(**kw):
                registrations.append(kw)
                return original_register(**kw)
            posix.register_at_fork = record_registration
        """)

    def test_runtime_ready_before_public_api_import(self):
        self.run_child("""
            import _cinderx, cinderjit
            assert sys.flags.no_site == 1
            assert _cinderx.is_frame_evaluator_installed()
            assert cinderjit.is_enabled()
            assert cinderjit.jit_frame_mode() == 0
            assert not {'os', 'site', 'array', 'cinderx'}.intersection(sys.modules)
            assert '_cinderx_auto_helpers' not in sys.modules
            bootstrap = sys.modules['_frozen_importlib']
            provider = bootstrap._find_and_load
            assert provider._cinderx_autojit_import_provider == 'find_and_load'
            assert provider.__code__.co_filename == '<frozen importlib._bootstrap>'
            assert provider.__wrapped__ is not provider
            import cinderx
            assert cinderx.is_initialized()
            cinderx.init()
            assert bootstrap._find_and_load is provider
            assert _cinderx._autojit_import_scope_depth() == 0
            assert _cinderx._autojit_setup_depth() == 0
        """)

    def test_preexisting_function_is_discovered_on_frame_entry(self):
        self.run_child("""
            import _cinderx, cinderjit, time
            time.sleep(0.11)  # Preserve the normal startup grace period.
            for _ in range(100):
                assert early([1.5, 2.5]) == 4.0
            assert cinderjit.is_jit_compiled(early)
            assert early in cinderjit.get_compiled_functions()
            assert _cinderx._get_trigger_stats()['machine_code_entries'] > 0
        """, before="""
            def early(values):
                return values[0] + values[1]
        """)

    def test_import_scope_unwinds_and_setup_providers_remain_enabled(self):
        self.run_child("""
            import _cinderx
            class Loader:
                def create_module(self, spec):
                    return None
                def exec_module(self, module):
                    assert _cinderx._autojit_import_scope_depth() > 0
                    raise ValueError('loader failed')
            class Finder:
                def find_spec(self, name, path=None, target=None):
                    if name == '_startup_scope_probe':
                        return sys.modules['_frozen_importlib'].ModuleSpec(name, Loader())
            sys.meta_path.insert(0, Finder())
            try:
                import _startup_scope_probe
            except ValueError as exc:
                assert str(exc) == 'loader failed'
            else:
                raise AssertionError('loader did not run')
            assert _cinderx._autojit_import_scope_depth() == 0
            import lib2to3.main, multiprocessing.pool
            assert '_cinderx_auto_helpers' in sys.modules
            assert lib2to3.main.main._cinderx_autojit_setup_provider == 'lib2to3_main'
            assert multiprocessing.pool.Pool.map._cinderx_autojit_setup_provider == 'multiprocessing_pool'
            assert _cinderx._autojit_setup_depth() == 0
        """)

    def test_setup_hooks_cover_modules_loaded_before_bootstrap(self):
        self.run_child("""
            import _cinderx
            assert '_cinderx_auto_helpers' in sys.modules
            assert lib2to3.main.main._cinderx_autojit_setup_provider == 'lib2to3_main'
            assert multiprocessing.pool.Pool.map._cinderx_autojit_setup_provider == 'multiprocessing_pool'
            assert _cinderx._autojit_import_scope_depth() == 0
            assert _cinderx._autojit_setup_depth() == 0
        """, before="""
            import lib2to3.main, multiprocessing.pool
        """)

    def test_import_warning_is_attributed_to_importer(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "_startup_warning_probe.py").write_text(
                "import warnings\nwarnings.warn('startup probe', UserWarning, stacklevel=2)\n"
            )
            self.run_child(f"""
                import warnings
                sys.path.insert(0, {directory!r})
                with warnings.catch_warnings(record=True) as caught:
                    warnings.simplefilter('always')
                    import _startup_warning_probe
                assert len(caught) == 1
                assert caught[0].filename == '<string>', caught[0].filename
            """)

    def test_environment_follows_os_after_its_first_import(self):
        self.run_child("""
            assert 'os' not in sys.modules
            assert _cinderx_auto._getenv('CINDERX_STARTUP_TEST_VALUE') == 'initial'
            assert _cinderx_auto._getenv('ABSENT_STARTUP_TEST_VALUE', 17) == 17
            import os
            os.environ['CINDERX_STARTUP_TEST_VALUE'] = 'changed'
            assert _cinderx_auto._getenv('CINDERX_STARTUP_TEST_VALUE') == 'changed'
            os.environ['CINDERX_STARTUP_TEST_VALUE'] = ''
            assert _cinderx_auto._getenv('CINDERX_STARTUP_TEST_VALUE') == ''
            del os.environ['CINDERX_STARTUP_TEST_VALUE']
            assert _cinderx_auto._getenv('CINDERX_STARTUP_TEST_VALUE') is None
        """, extra_env={"CINDERX_STARTUP_TEST_VALUE": "initial"})

    def test_late_array_import_still_compiles_and_handles_type_changes(self):
        self.run_child("""
            import _cinderx, cinderjit
            assert 'array' not in sys.modules
            from array import array
            def load(values):
                return values[0] + values[1]
            cinderjit.jit_suppress(load)
            values = array('d', [1.5, 2.5])
            for _ in range(100):
                assert load(values) == 4.0
            cinderjit.jit_unsuppress(load)
            assert cinderjit.force_compile(load)
            assert cinderjit.is_jit_compiled(load)
            for _ in range(20):
                assert load(values) == 4.0
            assert load(array('f', [3.5, 4.5])) == 8.0
            assert load(array('i', [3, 4])) == 7
            assert load([5, 6]) == 11
            try:
                load(array('d'))
            except IndexError:
                pass
            else:
                raise AssertionError('missing bounds check')
        """)

    def test_disabled_plugin_and_eval_only_modes(self):
        self.run_child("""
            assert '_cinderx' not in sys.modules
            assert 'cinderjit' not in sys.modules
        """, extra_env={"CINDERX_PLUGIN_ENABLE": "0"})
        self.run_child("""
            import _cinderx
            assert _cinderx.is_frame_evaluator_installed()
            assert 'cinderjit' not in sys.modules
            import cinderx
            assert cinderx.is_initialized()
        """, extra_env={"CINDERX_JIT_MODE": "off"})

    def test_disable_aliases_keep_their_behavior(self):
        self.run_child("""
            assert '_cinderx' not in sys.modules
        """, extra_env={"CINDERX_DISABLE": "yes"})
        for name in ("CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
            with self.subTest(name=name):
                self.run_child("""
                    import _cinderx
                    assert _cinderx.is_frame_evaluator_installed()
                    assert 'cinderjit' not in sys.modules
                """, extra_env={name: "true"})

    def test_requested_gate_statistics_are_not_deferred(self):
        result = self.run_child("""
            import _cinderx, cinderjit
            assert '_cinderx_auto_helpers' in sys.modules
            assert _cinderx.is_frame_evaluator_installed()
            assert cinderjit.is_enabled()
        """, extra_env={"CINDERX_AUTOJIT_GATE_STATS": "1"})
        self.assertIn("CINDERX_AUTOJIT_GATE_STATS: ", result.stderr)
        self.assertNotIn("Exception ignored in atexit callback", result.stderr)

    def test_unavailable_parallel_gc_does_not_change_gc_settings(self):
        self.run_child("""
            import _cinderx
            assert not _cinderx.has_parallel_gc()
            assert '_cinderx_auto_helpers' in sys.modules
            assert gc.get_threshold() == original_thresholds
        """, before="""
            import gc
            original_thresholds = gc.get_threshold()
        """, extra_env={
            "PARALLEL_GC_ENABLED": "1",
            "PARALLEL_GC_THRESHOLD_GEN0": "1234",
        })

    def test_shutdown_callback_uses_the_standard_registry(self):
        self.run_child("""
            import _cinderx
            initially_loaded = 'atexit' in sys.modules
            import sysconfig
            if sys.platform.startswith('linux') and sysconfig.get_config_var('Py_ENABLE_SHARED'):
                assert not initially_loaded
            import atexit
            assert atexit._ncallbacks() == 1
            events = []
            atexit.register(events.append, 'ordinary callback')
            atexit._run_exitfuncs()
            assert events == ['ordinary callback']
            assert atexit._ncallbacks() == 0
        """)

    def test_shutdown_callback_can_be_unregistered(self):
        self.run_child("""
            import _cinderx, atexit, weakref, gc
            callback = _cinderx._clear_strict_modules
            reference = weakref.ref(callback)
            del _cinderx._clear_strict_modules
            del callback
            gc.collect()
            assert reference() is not None
            atexit.unregister(reference())
            gc.collect()
            assert reference() is None
            atexit._run_exitfuncs()
        """)

    def test_preloaded_atexit_registration_is_respected(self):
        self.run_child("""
            import _cinderx, atexit
            assert registered == [_cinderx._clear_strict_modules]
            assert atexit._ncallbacks() == 33
            atexit._run_exitfuncs()
            assert events == list(reversed(range(32)))
        """, before="""
            import atexit
            events = []
            for i in range(32):
                atexit.register(lambda i=i: events.append(i))
            registered = []
            original_register = atexit.register
            def register(callback):
                registered.append(callback)
                return original_register(callback)
            atexit.register = register
        """)

    def test_shutdown_callback_pins_runtime_after_hooks_are_restored(self):
        self.run_child("""
            import gc, weakref
            reference = weakref.ref(_cinderx_auto._cinderx)
            sys._clear_type_cache = original_clear
            sys.settrace, sys.setprofile = original_trace, original_profile
            sys.modules.pop('cinderjit', None)
            sys.modules.pop('_cinderx')
            sys.modules.pop('_cinderx_auto')
            del _cinderx_auto
            gc.collect()
            assert reference() is not None
        """, before="""
            original_clear = sys._clear_type_cache
            original_trace, original_profile = sys.settrace, sys.setprofile
        """, extra_env={
            'CINDERX_AUTOJIT_IMPORT_PROVIDER': 'off',
            'CINDERX_AUTOJIT_SETUP_PROVIDER': 'off',
        })


if __name__ == "__main__":
    unittest.main()
