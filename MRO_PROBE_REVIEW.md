# MRO qualification ablation (experimental)

Base: reviewed attribute implementation at e42229df3 (implementation 2202166e).
Scope: Linux ARM CPython 3.11.6, main interpreter, JIT-off.

## Retained approach

Read the current interpreter's existing type lookup cache using the same version/name hit conditions as CPython 3.11.6 `_PyType_Lookup`:
https://github.com/python/cpython/blob/v3.11.6/Objects/typeobject.c

- No valid type tag or cached getter: use the reviewed full MRO qualification.
- Cached getter differs from the default getter: delegate the entire request to the original API before any lookup or callback.
- Cached default getter plus non-NULL cached handler: immediately own the handler and continue the existing attribute path.
- Any unproven case retains the complete MRO scan before extra type lookups.

The helper does not populate the host cache or own a persistent type/handler reference. It retains instance-dictionary guards, original descriptor binding, final exception handling, and handler lifetime protection. This is not an unchecked type-version memo: a cache miss is never used to infer namespace safety.

It relies on the exact CPython 3.11.6 internal type-cache layout and the GIL, like the existing version-pinned interpreter. New code requires review before merging into the previously reviewed branch.

## Controls and measurements

`ATTR_PATH_MRO_CACHE=0` keeps the full scan; `=1` enables the shortcut (default in this experimental build). Both retain correctness guards. No bypass-all-checks arm exists.

`_attr_mro_stats()` provides optional counters when ATTR_PATH_DIAGNOSTICS=1. calls = positive hits + cached fallbacks + full scans. accepted_base_visits counts only bases visited by successful full scans. These are helper calls, not every LOAD_ATTR execution or pure model-owned accesses. The specialization qualification still performs its original full guard.

Time runs disable counters. Same-binary scan/shortcut comparisons measure combined savings from qualification and redundant cached type lookups, not the isolated pure duration of MRO iteration.

## Regressions

Reuse type reentrancy, dictionary layout, startup, cache, ownership, and CPython call/trace suites. test_mro_cache.py also covers mutations, replacement with a general-key base, direct leaf/ancestor dictionary writes followed by host-cache clearing, custom MRO behavior, 300 temporary types with weakref collection, cached general namespaces versus builtin getattr, and reinstall. The negative-getter path checks both returned values and cached-fallback activity.

Earlier private-cache experiments (per-dictionary equality and a monotonic-version bound) passed targeted regressions but did not produce convincing net gains. They remain separate research artifacts; this branch does not contain their persistent cache tables.