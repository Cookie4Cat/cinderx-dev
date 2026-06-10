import os
import sys

if os.environ.get("CINDERX_PLUGIN_ENABLE", "0") == "1":
    import _cinderx  # noqa: F401
    import cinderjit  # noqa: F401

    _AUTOJIT_IMPORT_PROVIDER_MARKER = "_cinderx_autojit_import_provider"
    _AUTOJIT_SETUP_PROVIDER_MARKER = "_cinderx_autojit_setup_provider"
    _AUTOJIT_GATE_STATS_PREFIX = "CINDERX_AUTOJIT_GATE_STATS: "

    def _env_flag_enabled(name):
        return os.environ.get(name, "0").lower() in ("1", "true", "yes", "on")

    def _is_autojit_classification_value(value):
        return isinstance(value, str) and (
            value == "auto" or value.startswith("auto:")
        )

    def _autojit_import_provider():
        provider = os.environ.get("CINDERX_AUTOJIT_IMPORT_PROVIDER")
        if provider is not None:
            return provider
        if _is_autojit_classification_value(
            os.environ.get("PYTHONJITAUTO")
        ) or _is_autojit_classification_value(sys._xoptions.get("jit-auto")):
            return "find_and_load"
        return "off"

    def _maybe_enable_autojit_gate_stats():
        if not _env_flag_enabled("CINDERX_AUTOJIT_GATE_STATS"):
            return
        clear_stats = getattr(cinderjit, "_clear_autojit_gate_stats", None)
        read_stats = getattr(cinderjit, "_autojit_gate_stats", None)
        if clear_stats is None or read_stats is None:
            return

        clear_stats()

        def dump_autojit_gate_stats():
            import json

            payload = {
                "argv": sys.argv,
                "executable": sys.executable,
                "pid": os.getpid(),
                "stats": read_stats(),
            }
            line = json.dumps(payload, sort_keys=True)
            stats_file = os.environ.get("CINDERX_AUTOJIT_GATE_STATS_FILE")
            if stats_file:
                with open(stats_file, "a", encoding="utf-8") as out:
                    out.write(line)
                    out.write("\n")
            else:
                print(_AUTOJIT_GATE_STATS_PREFIX + line, file=sys.stderr)

        import atexit

        atexit.register(dump_autojit_gate_stats)

    def _make_autojit_setup_wrapper(original, provider):
        def wrapper(*args, **kwargs):
            _cinderx._autojit_setup_enter()
            try:
                return original(*args, **kwargs)
            finally:
                _cinderx._autojit_setup_leave()

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _maybe_install_autojit_setup_provider_for_module(fullname, provider=None):
        if provider is None:
            provider = os.environ.get("CINDERX_AUTOJIT_SETUP_PROVIDER", "")
        if provider in ("", "0", "off"):
            return
        if provider != "lib2to3_main" or fullname != "lib2to3.main":
            return

        module = sys.modules.get("lib2to3.main")
        if module is None:
            return

        current = getattr(module, "main", None)
        if current is None:
            return
        if getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None) == provider:
            return

        setattr(module, "main", _make_autojit_setup_wrapper(current, provider))

    def _make_autojit_import_wrapper(original, provider):
        setup_provider = os.environ.get("CINDERX_AUTOJIT_SETUP_PROVIDER", "")

        def wrapper(*args, **kwargs):
            _cinderx._autojit_import_enter()
            try:
                module = original(*args, **kwargs)
            finally:
                _cinderx._autojit_import_leave()
            if setup_provider and args and isinstance(args[0], str):
                _maybe_install_autojit_setup_provider_for_module(
                    args[0], setup_provider
                )
            return module

        setattr(wrapper, _AUTOJIT_IMPORT_PROVIDER_MARKER, provider)
        return wrapper

    def _install_autojit_import_provider():
        provider = _autojit_import_provider()
        if provider in ("", "0", "off"):
            return

        if provider == "builtins":
            target = sys.modules.get("builtins")
            attr = "__import__"
        elif provider == "find_and_load":
            target = sys.modules.get("importlib._bootstrap")
            attr = "_find_and_load"
        else:
            return

        if target is None:
            return

        current = getattr(target, attr)
        if getattr(current, _AUTOJIT_IMPORT_PROVIDER_MARKER, None) == provider:
            return

        setattr(target, attr, _make_autojit_import_wrapper(current, provider))

    def _maybe_enable_parallel_gc():
        if os.environ.get("PARALLEL_GC_ENABLED", "0") != "1":
            return
        if not _cinderx.has_parallel_gc():
            return

        import gc

        thresholds = gc.get_threshold()
        parallel_gc_threshold_gen0 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN0", thresholds[0])
        )
        parallel_gc_threshold_gen1 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN1", thresholds[1])
        )
        parallel_gc_threshold_gen2 = int(
            os.environ.get("PARALLEL_GC_THRESHOLD_GEN2", thresholds[2])
        )
        gc.set_threshold(
            parallel_gc_threshold_gen0,
            parallel_gc_threshold_gen1,
            parallel_gc_threshold_gen2,
        )

        parallel_gc_num_threads = int(os.environ.get("PARALLEL_GC_NUM_THREADS", "0"))
        parallel_gc_min_generation = int(
            os.environ.get("PARALLEL_GC_MIN_GENERATION", "2")
        )

        _cinderx.enable_parallel_gc(
            min_generation=parallel_gc_min_generation,
            num_threads=parallel_gc_num_threads,
        )

    _maybe_enable_autojit_gate_stats()
    _maybe_enable_parallel_gc()
    _install_autojit_import_provider()
    _maybe_install_autojit_setup_provider_for_module("lib2to3.main")
