// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// OSR Loop Header Secondary Entry Test Suite (Feature Item 2: OSR Compilation)
//
// Tests verify HIR-level OSR entry annotation (markOSREntries, extractOSRLiveIns)
// and per-backedge OSR entry stub generation (OSRMetadata, live-in mapping).
//
// Design references:
//   - docs/design/hot-loop-osr/hot-loop-osr-function-design.md (Feature Item 2)
//
// Build note: This file requires CINDERX_OSR_HEADERS_AVAILABLE=1 to compile.
// When the OSR headers are not available, all tests are disabled.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#ifndef CINDERX_OSR_HEADERS_AVAILABLE
#define CINDERX_OSR_HEADERS_AVAILABLE 0
#endif

#if CINDERX_OSR_HEADERS_AVAILABLE

#include "cinderx/Jit/osr.h"

#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit;
using namespace jit::hir;

namespace {

class OSRCompileTest : public RuntimeTest {};

bool isBackwardJump(const BytecodeInstruction& instr) {
  return instr.opcode() == JUMP_BACKWARD ||
      instr.opcode() == JUMP_BACKWARD_NO_INTERRUPT;
}

std::vector<BCOffset> rawBackedgeTargets(BorrowedRef<PyCodeObject> code) {
  std::vector<BCOffset> targets;
  for (auto instr : BytecodeInstructionBlock{code}) {
    if (isBackwardJump(instr)) {
      targets.emplace_back(instr.getJumpTarget());
    }
  }
  return targets;
}

std::size_t countOSREntries(Function& func) {
  std::size_t count = 0;
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      count += instr.IsOSREntry();
    }
  }
  return count;
}

DeoptBase* firstOSREntry(Function& func) {
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsOSREntry()) {
        return instr.asDeoptBase();
      }
    }
  }
  return nullptr;
}

bool hasOSREntryAfterSnapshot(Function& func, BCOffset target_offset) {
  for (auto& block : func.cfg.blocks) {
    auto it = block.begin();
    if (it == block.end() || !it->IsSnapshot()) {
      continue;
    }

    auto& snapshot = static_cast<Snapshot&>(*it);
    FrameState* fs = snapshot.frameState();
    if (fs == nullptr || fs->instrOffset() != target_offset) {
      continue;
    }

    ++it;
    return it != block.end() && it->IsOSREntry();
  }
  return false;
}

BasicBlock* blockWithEntrySnapshotAt(Function& func, BCOffset target_offset) {
  for (auto& block : func.cfg.blocks) {
    Snapshot* snapshot = block.entrySnapshot();
    if (snapshot == nullptr) {
      continue;
    }

    FrameState* fs = snapshot->frameState();
    if (fs != nullptr && fs->instrOffset() == target_offset) {
      return &block;
    }
  }
  return nullptr;
}

} // namespace

TEST_F(OSRCompileTest, CollectsWhileLoopHeaderTarget) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto raw_targets = rawBackedgeTargets(func->func_code);
  ASSERT_EQ(raw_targets.size(), 1);

  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets.front(), raw_targets.front());
}

TEST_F(OSRCompileTest, CollectsForLoopHeaderTargetBeforeEntryFiltering) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(xs):
    total = 0
    for x in xs:
        total += x
    return total
)",
      "test"));
  auto raw_targets = rawBackedgeTargets(func->func_code);
  ASSERT_EQ(raw_targets.size(), 1);

  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets.front(), raw_targets.front());
}

TEST_F(OSRCompileTest, DeduplicatesBackedgesWithTheSameLoopHeaderTarget) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
        if i & 1:
            continue
        i += 1
    return i
)",
      "test"));
  auto raw_targets = rawBackedgeTargets(func->func_code);
  ASSERT_GE(raw_targets.size(), 2);
  for (BCOffset target : raw_targets) {
    EXPECT_EQ(target, raw_targets.front());
  }

  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets.front(), raw_targets.front());
}

TEST_F(OSRCompileTest, CollectsExtendedArgBackedgeTarget) {
  std::string source = "def test(n):\n"
                       "    i = 0\n"
                       "    while i < n:\n";
  for (int i = 0; i < 320; ++i) {
    source += "        i += 1\n";
  }
  source += "    return i\n";

  Ref<PyFunctionObject> func(compileAndGet(source.c_str(), "test"));
  std::optional<BCOffset> extended_target;
  for (auto instr : BytecodeInstructionBlock{func->func_code}) {
    if (isBackwardJump(instr) && instr.baseOffset() != instr.opcodeOffset()) {
      extended_target = instr.getJumpTarget();
      break;
    }
  }
  ASSERT_TRUE(extended_target.has_value());

  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  EXPECT_EQ(targets.front(), *extended_target);
}

TEST_F(OSRCompileTest, MarksEligibleWhileLoopHeaderAsSecondaryEntry) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  BasicBlock* loop_header =
      blockWithEntrySnapshotAt(*irfunc, targets.front());
  ASSERT_NE(loop_header, nullptr);
  auto loop_header_in_edges = loop_header->in_edges();

  irfunc->markOSREntries(targets, func->func_code);

  EXPECT_TRUE(irfunc->hasOSREntries());
  EXPECT_EQ(countOSREntries(*irfunc), 1);
  EXPECT_TRUE(hasOSREntryAfterSnapshot(*irfunc, targets.front()));
  EXPECT_EQ(loop_header->in_edges(), loop_header_in_edges);
}

TEST_F(OSRCompileTest, RejectsForLoopHeaderWithLiveOperandStack) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(xs):
    total = 0
    for x in xs:
        total += x
    return total
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);

  irfunc->markOSREntries(targets, func->func_code);

  EXPECT_FALSE(irfunc->hasOSREntries());
  EXPECT_EQ(countOSREntries(*irfunc), 0);
}

TEST_F(OSRCompileTest, RejectsLoopHeaderInExceptionProtectedRange) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    try:
        while i < n:
            i += 1
    except Exception:
        return -1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);

  irfunc->markOSREntries(targets, func->func_code);

  EXPECT_FALSE(irfunc->hasOSREntries());
  EXPECT_EQ(countOSREntries(*irfunc), 0);
}

TEST_F(OSRCompileTest, AcceptsLoopHeaderAtExceptionProtectedRangeEnd) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
import dis

def test():
    while True:
        pass

# CPython exception table offsets are code units.  Cover [0, target) so the
# loop header is exactly the exclusive protected-range end.
target = next(
    instr.argval for instr in dis.get_instructions(test)
    if instr.opname.startswith("JUMP_BACKWARD")
)
test.__code__ = test.__code__.replace(
    co_exceptiontable=bytes((0x80, target // 2, 0x00, 0x00))
)
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);

  irfunc->markOSREntries(targets, func->func_code);

  EXPECT_TRUE(irfunc->hasOSREntries());
  EXPECT_EQ(countOSREntries(*irfunc), 1);
}

TEST_F(OSRCompileTest, RejectsLoopHeaderInLaterExceptionTableEntry) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
import dis

def test():
    while True:
        pass

target = next(
    instr.argval for instr in dis.get_instructions(test)
    if instr.opname.startswith("JUMP_BACKWARD")
)
# The first protected range ends before the target.  The second starts at the
# target so parsing must walk past the first exception table entry to reject it.
test.__code__ = test.__code__.replace(
    co_exceptiontable=bytes((
        0x80, 0x01, 0x00, 0x00,
        0x80 | (target // 2), 0x01, 0x00, 0x00,
    ))
)
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);

  irfunc->markOSREntries(targets, func->func_code);

  EXPECT_FALSE(irfunc->hasOSREntries());
  EXPECT_EQ(countOSREntries(*irfunc), 0);
}

TEST_F(OSRCompileTest, RefcountPassBindsFrameStateToOSREntry) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  irfunc->markOSREntries(targets, func->func_code);

  Compiler::runPasses(*irfunc, PassConfig::kMinimal);

  const DeoptBase* entry = firstOSREntry(*irfunc);
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(entry->frameState(), nullptr);
  EXPECT_EQ(entry->frameState()->instrOffset(), targets.front());
}

TEST_F(OSRCompileTest, RejectsBorrowedOSRLiveIn) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  irfunc->markOSREntries(targets, func->func_code);
  Compiler::runPasses(*irfunc, PassConfig::kMinimal);

  DeoptBase* entry = firstOSREntry(*irfunc);
  ASSERT_NE(entry, nullptr);
  ASSERT_FALSE(entry->live_regs().empty());
  entry->live_regs().front().ref_kind = RefKind::kBorrowed;

  irfunc->extractOSRLiveIns();

  EXPECT_FALSE(irfunc->hasOSREntries());
}

TEST_F(OSRCompileTest, RejectsPrimitiveOSRLiveIn) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  irfunc->markOSREntries(targets, func->func_code);
  Compiler::runPasses(*irfunc, PassConfig::kMinimal);

  DeoptBase* entry = firstOSREntry(*irfunc);
  ASSERT_NE(entry, nullptr);
  ASSERT_FALSE(entry->live_regs().empty());
  entry->live_regs().front().value_kind = ValueKind::kSigned;

  irfunc->extractOSRLiveIns();

  EXPECT_FALSE(irfunc->hasOSREntries());
}

TEST_F(OSRCompileTest, RejectsOSRLiveInWithoutLocalsplusSource) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto irfunc = buildHIR(func);
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);
  irfunc->markOSREntries(targets, func->func_code);
  Compiler::runPasses(*irfunc, PassConfig::kMinimal);

  DeoptBase* entry = firstOSREntry(*irfunc);
  ASSERT_NE(entry, nullptr);
  ASSERT_FALSE(entry->live_regs().empty());
  entry->live_regs().front().reg = irfunc->env.AllocateRegister();

  irfunc->extractOSRLiveIns();

  EXPECT_FALSE(irfunc->hasOSREntries());
}

#if defined(__aarch64__)

TEST_F(OSRCompileTest, CompileBuildsMetadataForLocalsplusLiveIns) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test(n):
    i = 0
    while i < n:
        i += 1
    return i
)",
      "test"));
  auto targets = collectBackedgeTargetOffsets(func->func_code);
  ASSERT_EQ(targets.size(), 1);

  auto compiled = Compiler().Compile(func);

  ASSERT_TRUE(compiled.has_value());
  EXPECT_TRUE(compiled->osr_aware);
  EXPECT_TRUE(compiled->has_osr_entries);
  ASSERT_NE(compiled->runtime, nullptr);
  const auto& metadatas = compiled->runtime->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  const auto& metadata = metadatas.front();
  EXPECT_EQ(metadata.target_offset, targets.front());
  EXPECT_TRUE(metadata.allReconstructible());
  EXPECT_FALSE(metadata.live_ins.empty());
  EXPECT_GE(metadata.entry_point_offset, 0);
  for (const auto& live_in : metadata.live_ins) {
    EXPECT_GE(live_in.localsplus_index, 0);
    EXPECT_EQ(live_in.stack_index, -1);
    EXPECT_TRUE(live_in.reconstructible);
    EXPECT_NE(live_in.destination.loc, codegen::PhyLocation::REG_INVALID);
  }
}

TEST_F(OSRCompileTest, CompileBuildsMetadataAndStubForEmptyLiveIns) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def test():
    while True:
        pass
)",
      "test"));

  auto compiled = Compiler().Compile(func);

  ASSERT_TRUE(compiled.has_value());
  EXPECT_TRUE(compiled->osr_aware);
  EXPECT_TRUE(compiled->has_osr_entries);
  ASSERT_NE(compiled->runtime, nullptr);
  const auto& metadatas = compiled->runtime->osrMetadatas();
  ASSERT_EQ(metadatas.size(), 1);
  EXPECT_TRUE(metadatas.front().live_ins.empty());
  EXPECT_GE(metadatas.front().entry_point_offset, 0);
}

#endif // __aarch64__

#endif // CINDERX_OSR_HEADERS_AVAILABLE
