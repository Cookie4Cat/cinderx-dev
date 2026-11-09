import sys

if sys.version_info[:2] == (3, 11):

    def _getenv(name, default=None):
        # Before os is imported, its future environment is the bootstrapped
        # platform module's snapshot. Once os exists, honor its live mapping.
        os_module = sys.modules.get("os")
        if os_module is not None and hasattr(os_module, "environ"):
            return os_module.environ.get(name, default)
        platform_module = sys.modules.get("posix") or sys.modules.get("nt")
        if platform_module is None:
            import os

            return os.environ.get(name, default)
        if sys.platform == "win32":
            return platform_module.environ.get(name, default)
        encoding = sys.getfilesystemencoding()
        value = platform_module.environ.get(name.encode(encoding, "surrogateescape"))
        return default if value is None else value.decode(encoding, "surrogateescape")
else:
    import os

    def _getenv(name, default=None):
        return os.environ.get(name, default)

def _env_flag_enabled(name):
    return _getenv(name, "0").lower() in ("1", "true", "yes", "on")

def _cinderx_plugin_enabled():
    return _getenv("CINDERX_PLUGIN_ENABLE", "0") == "1"

def _cinderx_force_disabled():
    return _env_flag_enabled("CINDERX_DISABLE")

def _cinderx_jit_disabled():
    return _env_flag_enabled("CINDERX_JIT_DISABLE") or _env_flag_enabled(
        "PYTHONJITDISABLE"
    )

def _cinderx_311_execute_mode():
    # On CPython 3.11 the JIT runs only in the explicit execute mode
    # (CINDERX_JIT_MODE=execute, or its test-only spelling "canary"); off,
    # observe and shadow publish no cinderjit module.  The native side
    # applies the same rule (observe.c), disable switches included.
    return _getenv("CINDERX_JIT_MODE", "off") in ("execute", "canary")

if _cinderx_plugin_enabled() and not _cinderx_force_disabled():
    import _cinderx  # noqa: F401

    if _cinderx_jit_disabled():
        cinderjit = None
    elif sys.version_info[:2] < (3, 12):
        if _cinderx_311_execute_mode():
            import cinderjit  # noqa: F401
        else:
            cinderjit = None
    else:
        import cinderjit  # noqa: F401

    if sys.version_info[:2] == (3, 11):
        # Startup control: the takeover happens only on explicit request --
        # plugin enabled plus CINDERX_EVAL_MODE=cinder.  Scoped to 3.11 so
        # other versions keep their existing startup behavior.
        _eval_mode = _getenv("CINDERX_EVAL_MODE", "stock")
        if _eval_mode not in ("stock", "cinder"):
            raise RuntimeError(
                "CINDERX_EVAL_MODE=%r is not accepted; this build takes "
                "'stock' or 'cinder'" % (_eval_mode,)
            )
        if _eval_mode == "cinder":
            # The native runtime is already initialized. Install its evaluator
            # here and perform the same provider/GC setup below, without loading
            # the public Python API solely to repeat this startup work.
            _cinderx.install_frame_evaluator()

    _AUTOJIT_IMPORT_PROVIDER_MARKER = "_cinderx_autojit_import_provider"
    _AUTOJIT_SETUP_PROVIDER_MARKER = "_cinderx_autojit_setup_provider"
    _AUTOJIT_GATE_STATS_PREFIX = "CINDERX_AUTOJIT_GATE_STATS: "

    def _is_autojit_classification_value(value):
        return isinstance(value, str) and (
            value == "auto" or value.startswith("auto:")
        )

    def _autojit_scheduling_is_configured():
        if _is_autojit_classification_value(
            _getenv("PYTHONJITAUTO")
        ) or _is_autojit_classification_value(sys._xoptions.get("jit-auto")):
            return True
        return (
            sys.version_info[:2] == (3, 11)
            and _eval_mode == "cinder"
            and _cinderx_311_execute_mode()
        )

    def _autojit_import_provider():
        provider = _getenv("CINDERX_AUTOJIT_IMPORT_PROVIDER")
        if provider is not None:
            return provider
        if _autojit_scheduling_is_configured():
            return "find_and_load"
        return "off"

    def _autojit_setup_provider():
        provider = _getenv("CINDERX_AUTOJIT_SETUP_PROVIDER")
        if provider is not None:
            return provider
        if _autojit_scheduling_is_configured():
            return "lib2to3_main,multiprocessing_pool"
        return "off"

    def _maybe_enable_autojit_gate_stats():
        if cinderjit is None or not _env_flag_enabled("CINDERX_AUTOJIT_GATE_STATS"):
            return
        import _cinderx_auto_helpers
        _cinderx_auto_helpers._maybe_enable_autojit_gate_stats()

    def _maybe_install_autojit_setup_provider_for_module(fullname, provider=None):
        if fullname not in ("lib2to3.main", "multiprocessing.pool"):
            return
        if fullname not in sys.modules:
            return
        import _cinderx_auto_helpers
        _cinderx_auto_helpers._maybe_install_autojit_setup_provider_for_module(
            fullname, provider
        )

    def _make_autojit_import_wrapper(original, provider):
        setup_provider = _autojit_setup_provider()
        if sys.version_info[:2] == (3, 11):
            enter = _cinderx._autojit_import_enter
            leave = _cinderx._autojit_import_leave
            install = _maybe_install_autojit_setup_provider_for_module

            def wrapper(*args, **kwargs):
                enter()
                try:
                    module = original(*args, **kwargs)
                finally:
                    leave()
                if setup_provider and args and isinstance(args[0], str):
                    install(args[0], setup_provider)
                return module

            code = wrapper.__code__.replace(
                co_filename="<frozen importlib._bootstrap>",
                co_firstlineno=3,
                co_qualname="_make.<locals>.wrapper",
            )
            wrapper = type(wrapper)(
                code,
                {"__builtins__": wrapper.__builtins__},
                "wrapper",
                None,
                wrapper.__closure__,
            )
            setattr(wrapper, _AUTOJIT_IMPORT_PROVIDER_MARKER, provider)
            setattr(wrapper, "__wrapped__", original)
            return wrapper

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
            if target is None and sys.version_info[:2] == (3, 11):
                target = sys.modules.get("_frozen_importlib")
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
        if _getenv("PARALLEL_GC_ENABLED", "0") != "1":
            return
        import _cinderx_auto_helpers
        _cinderx_auto_helpers._maybe_enable_parallel_gc()

    _maybe_enable_parallel_gc()
    if cinderjit is not None:
        _maybe_enable_autojit_gate_stats()
    if cinderjit is not None or (
        sys.version_info[:2] == (3, 11) and _eval_mode == "cinder"
    ):
        _install_autojit_import_provider()
        _maybe_install_autojit_setup_provider_for_module("lib2to3.main")
        _maybe_install_autojit_setup_provider_for_module("multiprocessing.pool")
