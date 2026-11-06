# CPython 3.11.6 attribute path optimization

Base: openeuler/cinderx dev bdba8d75f49d441c929981ffed5fdbfb9545ed58.
Port the V24 generic-miss suppression and getattr-only LOAD_ATTR/LOAD_METHOD specialization. This is a source transplant onto current dev, not an overwrite of the old evaluator. Current dev call routing, JIT machinery and key-version policies are preserved. Includes the V24 portable global-name cache and guarded JIT-off inline-call support. Inline calls are disabled in all observing/compiling/executing JIT modes, preserving current-dev heat accounting and compiled artifact dispatch.

The upstream source files remain unchanged. Configure checks upstream hashes and applies patches to build-directory copies. Non-string-key dictionaries stay on the generic path.

Linux ARM CPython 3.11.6. Experimental runtime controls:

```
ATTR_PATH_MODE=off python workload.py
ATTR_PATH_MODE=cinderx_control python workload.py
ATTR_PATH_MODE=cinderx python workload.py
ATTR_PATH_MODE=cinderx python -c "import _cinderx_auto; print(_cinderx_auto._verify())"
```

The explicit attribute modes disable JIT. The control keeps the evaluator but disables both attribute optimizations. Explicit `ATTR_PATH_SPECIALIZE=0` allows no-exception-only ablation. Diagnostics are optional (`ATTR_PATH_DIAGNOSTICS=1`) and must be off while timing. Do not combine with slot_native.

Tests: test_specga.py covers warmed specialization and adversarial mutations; semantic_worker.py compares attribute results, errors and side effects. Run each mode in a separate process and compare `cases`, not runtime metadata.

Performance trials use actual model Python methods with mock compute. They are not real NPU FREE TIME. The supported candidate uses PGO+LTO; its training workload exercises interpreter calls, attribute hits/misses, JSON, pickle and deepcopy. Compare against a matching no-support arm in the same binary when attributing the support patches.
Additional ablation controls: ATTR_PATH_GLOBAL_CACHE=0/1 and ATTR_PATH_INLINE_CALL=0/1 (default 1 in explicit attribute modes). Both remain optional; cache validation uses full 64-bit dictionary content versions, never a private keys-version allocator.

## Runtime interfaces

ATTR_PATH_NOEXC selects ordinary-miss suppression; ATTR_PATH_DIAGNOSTICS controls counting. These two settings are captured after the native main-interpreter and supported-version admission checks, so later environment changes do not alter reinstallation. ATTR_PATH_MODE remains the user-facing three-mode entry point. Defaults are resolved from that process-local mode and are not exported to the environment. Explicit ATTR_PATH_* overrides remain supported.

_attr_path_status() returns six configuration fields: no_exception, specialization, split_dict, global_cache, inline_call, diagnostics. inline_call is an enable flag, not evidence of a taken fast path. _attr_path_counters() returns None with diagnostics off; otherwise it returns attribute_hits, attribute_misses, attribute_fallbacks, attribute_errors, global_hits, global_misses. Internal counting and lookup branches are unchanged. The old experimental environment names and combined _interp_getattr_stats311 interface are removed; consumers must use the new interfaces.

Patch overview: ../../cinderx/Interpreter/3.11/attr-patches/README.md (from repository root, cinderx/Interpreter/3.11/attr-patches/README.md).

## Review fixes

General-key managed dictionaries and non-managed instance-dictionary layouts take the original attribute API before any dictionary comparison. Explicit attribute startup verifies all six resolved flags and aborts through SystemExit on failure, including real .pth activation. Removal checks the owner interpreter before changing cache state. The source distribution includes the four patches, apply.cmake, native linker version script and .pth bootstrap inputs.

Regressions: test_dict_attributeerror.py, test_startup.py and test_subinterpreter_owner.py. The startup matrix exercises ordinary/off/full/control parents spawning off/full/control children, explicit overrides, invalid startup, and actual-state mismatch detection.

Reverse initialization-order regression: test_first_import_order.py execute and test_first_import_order.py cinderx_control must each run in a fresh process with native CinderX initially unloaded. The execute case deliberately forces and executes one small compiled function; it is not a model performance test. Patch-only incremental build regression: python tools/attr_path/test_patch_dependencies.py.
