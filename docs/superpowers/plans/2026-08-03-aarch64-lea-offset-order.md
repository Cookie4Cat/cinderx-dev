# AArch64 Large-Lea Offset Ordering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ensure an unencodable AArch64 Lea offset is defined by a Move before the generated Add uses its virtual register.

**Architecture:** Keep the existing target-selection lowering and original Lea insertion anchor. Materialize an unencodable offset before allocating the single Add, then link the Add to that Move; retain the immediate path without an extra instruction.

**Tech Stack:** C++17, CinderX LIR, GoogleTest RuntimeTests, AArch64 GCC/GCov gates, clang-format, GitCode Jenkins.

---

## File Structure

- Modify `cinderx/RuntimeTests/lir_target_select_test.cpp`: strengthen the
  existing `0x100001` regression to check exact def-before-use ordering and
  the Add-to-Move link.
- Modify `cinderx/Jit/lir/target_select.cpp`: prepare any required offset Move
  before creating the Add, without changing other Lea lowering paths.
- Update `findings.md` and `progress.md` outside the code commit as the local
  evidence record for RED/GREEN, gates, commit SHA, and PR CI.

### Task 1: Lock Down the Use-Before-Definition Regression

**Files:**
- Modify: `cinderx/RuntimeTests/lir_target_select_test.cpp:228-246`
- Test: `cinderx/RuntimeTests/lir_target_select_test.cpp`

- [x] **Step 1: Replace the presence-only large-offset assertions with an instruction-order test**

Keep the existing LIR input and replace the body after it with:

```cpp
  std::unique_ptr<Function> func = Parser().parse(lir_input_str);
  selectTargetOpcodes(func.get());

  ASSERT_EQ(func->basicblocks().size(), 1);
  std::vector<Instruction*> instrs =
      collectTargetSelectInstrs(*func->basicblocks()[0]);
  ASSERT_EQ(instrs.size(), 8);

  Instruction* offset_move = instrs[4];
  ASSERT_TRUE(offset_move->isMove());
  ASSERT_TRUE(offset_move->getInput(0)->isImm());
  EXPECT_EQ(offset_move->getInput(0)->getConstant(), 0x100001);

  Instruction* add = instrs[5];
  ASSERT_TRUE(add->isAdd());
  ASSERT_EQ(add->getNumInputs(), 2);
  ASSERT_TRUE(add->getInput(1)->isLinked());
  EXPECT_EQ(
      static_cast<LinkedOperand*>(add->getInput(1))->getLinkedInstr(),
      offset_move);

  EXPECT_TRUE(instrs[6]->isMove());
```

- [x] **Step 2: Run the focused test on the AArch64 validation host and verify RED**

Run from the exact candidate checkout after rebuilding RuntimeTests:

```bash
runtime-tests-build/cinderx/RuntimeTests/runtime_tests \
  --gtest_filter=LIRTargetSelectTest.SelectsMulAddForLeaLargeMultiplierWithLargeOffset
```

Expected: FAIL because `instrs[4]` is the Add on the current implementation,
proving the generated Add precedes the offset Move.

- [x] **Step 3: Record the RED artifact and leave production code unchanged**

Record the exact commit, command, failing assertion, and output path in
`findings.md`. Confirm `git diff --name-only` lists only the RuntimeTest before
starting Task 2.

### Task 2: Materialize the Offset Before Its Add

**Files:**
- Modify: `cinderx/Jit/lir/target_select.cpp:313-331`
- Test: `cinderx/RuntimeTests/lir_target_select_test.cpp`

- [x] **Step 1: Prepare an optional offset Move before allocating Add**

Replace the current Add-first offset block with:

```cpp
  Instruction* final_result = muladd;
  if (offset != 0) {
    uint64_t offset_value = static_cast<uint64_t>(static_cast<int64_t>(offset));

    Instruction* offset_move = nullptr;
    if (!asmjit::arm::Utils::isAddSubImm(offset_value)) {
      offset_move = block->allocateInstrBefore(
          instr_iter,
          Instruction::kMove,
          OutVReg{DataType::k64bit},
          Imm{offset_value, DataType::k64bit});
    }

    Instruction* add = block->allocateInstrBefore(
        instr_iter, Instruction::kAdd, OutVReg{DataType::k64bit}, VReg{muladd});
    if (offset_move == nullptr) {
      add->addOperands(Imm{offset_value, DataType::k64bit});
    } else {
      add->addOperands(VReg{offset_move});
    }

    final_result = add;
  }
```

Do not change multiplier selection, base/index ownership, encodable offset
semantics, final Move linkage, or any non-Lea opcode path.

- [x] **Step 2: Rebuild and rerun the focused test to verify GREEN**

Run:

```bash
runtime-tests-build/cinderx/RuntimeTests/runtime_tests \
  --gtest_filter=LIRTargetSelectTest.SelectsMulAddForLeaLargeMultiplierWithLargeOffset
```

Expected: PASS, with the offset Move at instruction index 4 and its Add user at
index 5.

- [x] **Step 3: Run both focused Lea tests**

Run:

```bash
runtime-tests-build/cinderx/RuntimeTests/runtime_tests \
  --gtest_filter='LIRTargetSelectTest.SelectsMulAddForLeaLargeMultiplier*'
```

Expected: 2 tests run, 2 tests pass. This verifies both encodable `0x8` and
unencodable `0x100001` offsets.

### Task 3: Verify Formatting and AArch64 Functional Gates

**Files:**
- Verify: `cinderx/Jit/lir/target_select.cpp`
- Verify: `cinderx/RuntimeTests/lir_target_select_test.cpp`

- [ ] **Step 1: Run diff and format checks**

Run:

```bash
git diff --check
clang-format --dry-run --Werror \
  cinderx/Jit/lir/target_select.cpp \
  cinderx/RuntimeTests/lir_target_select_test.cpp
```

Expected: both commands exit 0 and produce no suggested format diff.

- [x] **Step 2: Run RuntimeTests coverage gate**

Run from the exact candidate AArch64 checkout:

```bash
python3.14 ci_pipeline/run_gate.py runtime --coverage
```

Expected: gate summary reports 1/1 passed and all coverage thresholds passed.

- [x] **Step 3: Run cinderx_local gate**

Run:

```bash
python3.14 ci_pipeline/run_gate.py cinderx_local
```

Expected: gate summary reports 4/4 jobs passed, including both selected
`Lib/test` jobs.

- [x] **Step 4: Review the final two-file behavior diff**

Confirm the production diff only reorders offset materialization before Add,
the test directly checks the link and order, and no unrelated file is staged.

### Task 4: Commit, Push, and Close PR Validation

**Files:**
- Commit: `cinderx/Jit/lir/target_select.cpp`
- Commit: `cinderx/RuntimeTests/lir_target_select_test.cpp`
- Preserve: `docs/superpowers/specs/2026-08-03-aarch64-lea-offset-order-design.md`
- Preserve: `docs/superpowers/plans/2026-08-03-aarch64-lea-offset-order.md`

- [ ] **Step 1: Create one signed-off correctness commit**

Stage only the production and RuntimeTest files and commit with:

```text
fix: define large Lea offsets before use

Materialize unencodable AArch64 Lea offsets before creating the Add
that consumes them. Extend the focused target-selection test to verify
the definition order and exact linked operand.

Signed-off-by: xiaoji113 <113xiaoji@163.com>
```

Expected: commit message lines are at most 72 characters and the commit contains
exactly two files.

- [ ] **Step 2: Push the branch as a normal fast-forward**

Verify the remote branch still matches the previous local ancestor, then push
`codex/t10-mr3-aarch64-target-select-legalization` to `xiaoji113/cinderx`.

Expected: GitCode hooks pass and PR #165 updates to the new exact tip.

- [ ] **Step 3: Monitor replacement PR CI to completion**

Follow the new openEuler CI bot run for PR #165 and require SUCCESS from the
wrapper, x86_64, AArch64, DT, and UBSCore pre-commit jobs. If any job fails,
read only that run's logs and return to the relevant plan task.

- [ ] **Step 4: Update evidence and PR metadata**

Record the final tip, RED/GREEN paths, AArch64 gate summaries, and CI build IDs
in `findings.md` and `progress.md`. Update the PR description so it distinguishes
the original pyperformance implementation tip from the correctness follow-up;
do not claim a new performance result for the two-file ordering fix.
