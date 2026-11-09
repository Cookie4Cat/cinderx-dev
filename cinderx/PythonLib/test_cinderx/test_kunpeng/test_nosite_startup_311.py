# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Real -S startup: native readiness, import scopes and lazy metadata."""

import importlib.util
import os
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

    def test_runtime_ready_before_public_api_import(self):
        self.run_child("""
            import _cinderx, cinderjit
            assert sys.flags.no_site == 1
            assert _cinderx.is_frame_evaluator_installed()
            assert cinderjit.is_enabled()
            assert cinderjit.jit_frame_mode() == 0
            assert not {'os', 'site', 'array', 'cinderx'}.intersection(sys.modules)
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
            assert lib2to3.main.main._cinderx_autojit_setup_provider == 'lib2to3_main'
            assert multiprocessing.pool.Pool.map._cinderx_autojit_setup_provider == 'multiprocessing_pool'
            assert _cinderx._autojit_setup_depth() == 0
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


if __name__ == "__main__":
    unittest.main()
