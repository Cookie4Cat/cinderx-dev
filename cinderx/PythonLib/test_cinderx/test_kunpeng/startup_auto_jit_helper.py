import os
import sys
import types


def auto_jit_target(value):
    return value


auto_mode = os.environ.get("PYTHONJITAUTO", "").startswith("auto")
call_count = 3 if auto_mode else 10
result = None
for value in range(call_count):
    result = auto_jit_target(value)

cinderx = sys.modules.get("cinderx")
cinderjit = sys.modules.get("cinderjit")
expect_lazy_bootstrap = os.environ.get("EXPECT_CINDERX_EAGER_BOOTSTRAP") != "1"

assert "_cinderx_auto" in sys.modules, "_cinderx_auto startup hook was not loaded"

if os.environ.get("CINDERX_PLUGIN_ENABLE", "0") == "1":
    assert cinderjit is not None, "cinderjit was not auto-loaded"
    if expect_lazy_bootstrap:
        assert cinderx is None, "cinderx package should not be auto-loaded"
        import _cinderx  # noqa: F401
    else:
        assert cinderx is not None, "cinderx was not auto-loaded"
        assert cinderx.is_initialized(), "cinderx was not initialized"
    assert cinderjit.is_enabled(), "cinderx JIT is not enabled"
    if auto_mode:
        import _cinderx

        assert (
            not _cinderx.is_frame_evaluator_installed()
        ), "AutoJIT classification should not install the frame evaluator"
        assert not cinderjit.is_jit_compiled(
            auto_jit_target
        ), "auto_jit_target should be deferred by AutoJIT classification"
        assert cinderjit.count_interpreted_calls(auto_jit_target) == call_count
    else:
        assert cinderjit.is_jit_compiled(
            auto_jit_target
        ), "auto_jit_target was not compiled"
        assert cinderjit.get_compiled_size(auto_jit_target) > 0

    provider = os.environ.get("CINDERX_AUTOJIT_IMPORT_PROVIDER", "find_and_load")
    if provider in {"builtins", "find_and_load"}:
        import _cinderx

        if provider == "builtins":
            wrapped = sys.modules["builtins"].__import__
        else:
            wrapped = sys.modules["importlib._bootstrap"]._find_and_load
        assert (
            getattr(wrapped, "_cinderx_autojit_import_provider", None) == provider
        ), f"{provider} import provider was not installed"

        observed_depths = []

        class ProbeFinder:
            def find_spec(self, fullname, path=None, target=None):
                if fullname == f"startup_auto_jit_probe_{provider}":
                    observed_depths.append(
                        (
                            _cinderx._autojit_import_depth(),
                            _cinderx._autojit_import_scope_depth(),
                            _cinderx._autojit_setup_depth(),
                        )
                    )
                return None

        finder = ProbeFinder()
        sys.meta_path.insert(0, finder)
        try:
            try:
                __import__(f"startup_auto_jit_probe_{provider}")
            except ModuleNotFoundError:
                pass
        finally:
            sys.meta_path.remove(finder)

        assert observed_depths, "import provider probe did not run"
        assert all(depth[0] > 0 for depth in observed_depths), observed_depths
        assert all(depth[1] > 0 for depth in observed_depths), observed_depths
        assert all(depth[2] == 0 for depth in observed_depths), observed_depths
        assert _cinderx._autojit_import_depth() == 0
        assert _cinderx._autojit_import_scope_depth() == 0
        assert _cinderx._autojit_setup_depth() == 0

    setup_provider = os.environ.get("CINDERX_AUTOJIT_SETUP_PROVIDER", "")
    if setup_provider == "lib2to3_main":
        if cinderx is None:
            import cinderx

        observed_depths = []

        def main():
            observed_depths.append(
                (
                    _cinderx._autojit_import_depth(),
                    _cinderx._autojit_import_scope_depth(),
                    _cinderx._autojit_setup_depth(),
                )
            )
            return 42

        module = types.ModuleType("lib2to3.main")
        module.main = main
        sys.modules["lib2to3.main"] = module
        cinderx._maybe_install_autojit_setup_provider_for_module("lib2to3.main")

        assert (
            getattr(module.main, "_cinderx_autojit_setup_provider", None)
            == setup_provider
        ), "lib2to3 setup provider was not installed"
        assert module.main() == 42
        assert observed_depths and observed_depths[0][0] > 0, observed_depths
        assert observed_depths[0][1] == 0, observed_depths
        assert observed_depths[0][2] > 0, observed_depths
        assert _cinderx._autojit_import_depth() == 0
        assert _cinderx._autojit_import_scope_depth() == 0
        assert _cinderx._autojit_setup_depth() == 0
else:
    assert cinderx is None, "cinderx was auto-loaded"
    assert cinderjit is None, "cinderjit was auto-loaded"
assert result == call_count - 1
