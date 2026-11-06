# CPython 3.11.6 attribute path spike

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
