# CPython 3.11 A1 refusal and coverage infrastructure

## Refusal pipeline

The public `cinderjit.force_compile()` API preserves its historical result
surface.  On 3.11 it checks `Ci_JitShell311_ExecuteRefusal()` before entering
the compiler, but most preflight failures are intentionally flattened to
`PYJIT_RESULT_CANNOT_SPECIALIZE`.  During compilation,
`Compiler::Compile()` may publish `REFUSE_SHAPE_SPECULATIVE_GUARD` through
`Ci_JitShell311_SetExecuteRefusal()`; the request scheduler consumes it with
`Ci_JitShell311_TakeExecuteRefusal()`.  If no compiler-stage reason exists,
the stable failure class is `SUPPORTED_OPCODE_FAILURE`.

The private 3.11-only `cinderjit._jit311_compile_diagnostic(function)` closes
the testability gap without changing `force_compile()`.  It returns:

```text
eligible  compile-time capability eligibility
compiled  whether the function has a published artifact after the attempt
phase     preflight, compiler, or runtime
reason    stable symbolic reason, or null on success
```

Preflight reasons come directly from the code/shape registries.  Compiler
reasons come from the one-shot execute-refusal channel.  Runtime states are
reported before compilation, so tracing, profiling, a foreign evaluator, and
a paused JIT cannot create an artifact behind a refusal result.  No acceptance
decision parses log text.

## Compile-All classification

`a1_compile_all_hook.py` is staged through `sitecustomize`; it never edits
CPython tests.  It attributes work from regrtest's `--worker-args`, scans the
real target module (including TestCase methods), and records TestCase and
doctest execution windows.  `a1_report.py` deduplicates functions by source
identity, checks every refusal against the capability manifest, and assigns
each of the frozen 72 modules exactly one of:

- `JIT_EXECUTED`
- `EXPECTED_SAFE_REFUSAL`
- `RUNTIME_FALLBACK`
- `UNCOVERED`

Unknown and unexpected refusals remain distinct and both fail closed.

## Warm specialization

`a1_warm_specialization.toml` is the closed 17-family contract.
`a1_warm.py` proves the expected adaptive opcode, typed compile/refusal result,
machine entry for W-JIT, and semantic result.  `BINARY_SUBSCR` and
`STORE_SUBSCR` are explicitly W-SAFE-REFUSE with
`REFUSE_SHAPE_EXECUTE_SURFACE`; no function-name allowlist is involved.
