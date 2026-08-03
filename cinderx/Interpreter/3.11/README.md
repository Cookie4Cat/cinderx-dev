# Standalone CPython 3.11 interpreter

This directory builds the CPython 3.11.6 evaluator as an independent static
library. It does not link the evaluator into `_cinderx`, install a PEP 523
hook, expose a Python API, or enable any CinderX JIT path.

The vendored files under `upstream/` and the interpreter-local Borrow output
are derived from the `Python-3.11.6` tree produced by rpmbuild of
`python3-3.11.6-34.oe2403sp3.src.rpm`. Their pinned digests are recorded in
`upstream/SHA256SUMS`; source-consistency enforcement belongs in a future CI
gate rather than this compile-only target.

Build it with:

```sh
cmake -S cinderx/Interpreter/3.11 -B build/interpreter-311 \
  -DPython_EXECUTABLE=/usr/bin/python3.11
cmake --build build/interpreter-311 --target \
  cinderx_cpython311_interpreter -j
```

The archive deliberately leaves integration-time CPython private dependencies
unresolved. Supplying those dependencies and linking or installing the
evaluator are responsibilities of the later CinderX integration change.
