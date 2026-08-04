# AArch64 Large-Lea Offset Ordering Fix

## Context

`selectA64LeaLargeMultiplier()` lowers an AArch64 `Lea` whose multiplier is
at least 4 into a scale `Move`, `MulAdd`, optional offset `Add`, and final
`Move`. An offset that cannot be encoded by AArch64 add/sub must first be
materialized by a separate `Move`.

`BasicBlock::allocateInstrBefore()` inserts with
`std::list::emplace(iter, ...)`. When multiple instructions use the same Lea
iterator as their insertion anchor, later insertions are placed after earlier
insertions but still before the Lea.

## Confirmed Failure

The current unencodable-offset path creates `Add` first and `offset_move`
second, both before the original Lea iterator. The resulting order is:

1. scale `Move`
2. `MulAdd`
3. `Add` using `offset_move`
4. `offset_move`
5. final `Move`

This is a use-before-definition in LIR. It can give liveness and register
allocation an invalid definition range. The existing test with offset
`0x100001` verifies only that the expected opcodes exist, so it does not catch
their order or link relationship.

## Chosen Design

Construct the offset operand before creating the `Add`:

- For an encodable offset, create an immediate operand without inserting an
  instruction.
- For an unencodable offset, insert `offset_move` before the original Lea and
  create a linked operand referring to its output.
- Create one `Add` after that preparation and append the prepared operand.

This preserves the existing encodable-offset behavior, avoids duplicating Add
construction, and guarantees `offset_move` precedes its use because both
instructions retain the original Lea as their insertion anchor.

## Verification Design

Extend the existing large-unencodable-offset test rather than adding a
duplicate scenario. The test will inspect the selected instruction list and
assert that:

- the offset materialization `Move` precedes the `Add`;
- the Add offset input is linked to that exact Move;
- the original Lea is still lowered to a final Move.

The new assertions must fail on the current implementation and pass after the
production change. Then run the focused RuntimeTest, the repository's required
AArch64 functional gates, formatting checks, and PR CI.

## Scope

Only `selectA64LeaLargeMultiplier()` and its existing focused RuntimeTest are
in scope. There is no opcode redesign, register allocator change, unrelated
target-selection refactor, or new performance claim. The existing formal
pyperformance result remains applicable because the repair is correctness-only
and must be separately validated for behavioral safety.
