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

    def _autojit_setup_provider_tokens(provider=None):
        if provider is None:
            provider = _autojit_setup_provider()
        if provider in ("", "0", "off"):
            return ()
        return tuple(
            token.strip()
            for token in provider.replace("+", ",").split(",")
            if token.strip() and token.strip() not in ("0", "off")
        )

    def _maybe_enable_autojit_gate_stats():
        if cinderjit is None:
            return
        if not _env_flag_enabled("CINDERX_AUTOJIT_GATE_STATS"):
            return
        clear_stats = getattr(cinderjit, "_clear_autojit_gate_stats", None)
        read_stats = getattr(cinderjit, "_autojit_gate_stats", None)
        if clear_stats is None or read_stats is None:
            return

        clear_stats()

        def dump_autojit_gate_stats():
            import json
            import os

            payload = {
                "argv": sys.argv,
                "executable": sys.executable,
                "pid": os.getpid(),
                "stats": read_stats(),
            }
            line = json.dumps(payload, sort_keys=True)
            stats_file = _getenv("CINDERX_AUTOJIT_GATE_STATS_FILE")
            if stats_file:
                with open(stats_file, "a", encoding="utf-8") as out:
                    out.write(line)
                    out.write("\n")
            else:
                print(_AUTOJIT_GATE_STATS_PREFIX + line, file=sys.stderr)

        import atexit

        atexit.register(dump_autojit_gate_stats)

    def _autojit_setup_predicate_matches(predicate, args):
        if predicate is None:
            return True
        if not args:
            return False
        return predicate(args[0])

    def _make_autojit_setup_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            _cinderx._autojit_setup_enter()
            try:
                return original(*args, **kwargs)
            finally:
                _cinderx._autojit_setup_leave()

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _make_autojit_setup_enter_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            _cinderx._autojit_setup_enter()
            try:
                return original(*args, **kwargs)
            except BaseException:
                _cinderx._autojit_setup_leave()
                raise

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _make_autojit_setup_leave_wrapper(original, provider, predicate=None):
        def wrapper(*args, **kwargs):
            if not _autojit_setup_predicate_matches(predicate, args):
                return original(*args, **kwargs)
            try:
                return original(*args, **kwargs)
            finally:
                _cinderx._autojit_setup_leave()

        setattr(wrapper, _AUTOJIT_SETUP_PROVIDER_MARKER, provider)
        setattr(wrapper, "__wrapped__", original)
        return wrapper

    def _is_process_pool_instance(obj):
        cls = type(obj)
        return (
            getattr(cls, "__module__", None) == "multiprocessing.pool"
            and getattr(cls, "__name__", None) == "Pool"
        )

    def _wrap_autojit_setup_attr(target, attr, provider, make_wrapper, predicate=None):
        current = getattr(target, attr, None)
        if current is None:
            return
        if getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None) == provider:
            return
        setattr(target, attr, make_wrapper(current, provider, predicate))

    def _install_autojit_multiprocessing_pool_provider(module):
        pool = getattr(module, "Pool", None)
        if pool is not None:
            _wrap_autojit_setup_attr(
                pool,
                "__init__",
                "multiprocessing_pool",
                _make_autojit_setup_wrapper,
                _is_process_pool_instance,
            )
            _wrap_autojit_setup_attr(
                pool,
                "__enter__",
                "multiprocessing_pool",
                _make_autojit_setup_enter_wrapper,
                _is_process_pool_instance,
            )
            _wrap_autojit_setup_attr(
                pool,
                "__exit__",
                "multiprocessing_pool",
                _make_autojit_setup_leave_wrapper,
                _is_process_pool_instance,
            )
            for attr in (
                "map",
                "imap",
                "imap_unordered",
                "starmap",
                "map_async",
                "starmap_async",
            ):
                _wrap_autojit_setup_attr(
                    pool,
                    attr,
                    "multiprocessing_pool",
                    _make_autojit_setup_wrapper,
                    _is_process_pool_instance,
                )

    def _maybe_install_autojit_setup_provider_for_module(fullname, provider=None):
        providers = _autojit_setup_provider_tokens(provider)
        if not providers:
            return
        if "lib2to3_main" in providers and fullname == "lib2to3.main":
            module = sys.modules.get("lib2to3.main")
            if module is None:
                return

            current = getattr(module, "main", None)
            if current is None:
                return
            if (
                getattr(current, _AUTOJIT_SETUP_PROVIDER_MARKER, None)
                != "lib2to3_main"
            ):
                setattr(
                    module,
                    "main",
                    _make_autojit_setup_wrapper(current, "lib2to3_main"),
                )

        if "multiprocessing_pool" in providers and fullname == "multiprocessing.pool":
            module = sys.modules.get("multiprocessing.pool")
            if module is not None:
                _install_autojit_multiprocessing_pool_provider(module)

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
        if not _cinderx.has_parallel_gc():
            return

        import gc

        thresholds = gc.get_threshold()
        parallel_gc_threshold_gen0 = int(
            _getenv("PARALLEL_GC_THRESHOLD_GEN0", thresholds[0])
        )
        parallel_gc_threshold_gen1 = int(
            _getenv("PARALLEL_GC_THRESHOLD_GEN1", thresholds[1])
        )
        parallel_gc_threshold_gen2 = int(
            _getenv("PARALLEL_GC_THRESHOLD_GEN2", thresholds[2])
        )
        gc.set_threshold(
            parallel_gc_threshold_gen0,
            parallel_gc_threshold_gen1,
            parallel_gc_threshold_gen2,
        )

        parallel_gc_num_threads = int(_getenv("PARALLEL_GC_NUM_THREADS", "0"))
        parallel_gc_min_generation = int(
            _getenv("PARALLEL_GC_MIN_GENERATION", "2")
        )

        _cinderx.enable_parallel_gc(
            min_generation=parallel_gc_min_generation,
            num_threads=parallel_gc_num_threads,
        )

    _maybe_enable_parallel_gc()
    if cinderjit is not None:
        _maybe_enable_autojit_gate_stats()
    if cinderjit is not None or (
        sys.version_info[:2] == (3, 11) and _eval_mode == "cinder"
    ):
        _install_autojit_import_provider()
        _maybe_install_autojit_setup_provider_for_module("lib2to3.main")
        _maybe_install_autojit_setup_provider_for_module("multiprocessing.pool")
