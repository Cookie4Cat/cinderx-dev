// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/type.h"

#include <cstddef>
#include <optional>

namespace jit::hir {

// TreeIterPhase encodes the current state of the state machine for each
// resume.
enum class TreeIterPhase : int32_t {
  kLeft = 0,
  kYield = 1,
  kRight = 2,
  kBacktrack = 3,
  kExit = 4,
};

// Holds the results of TreeIter pattern matching.  All pointers are borrowed
// from the Function being matched — they are invalidated once the CFG is
// rewritten.
struct TreeIterMatch {
  Register* self_reg{nullptr};
  Instr* initial_yield{nullptr};
  FrameState* yield_frame_state{nullptr};
  Type exact_node_type{TTop};

  std::size_t left_offset{0};
  std::size_t right_offset{0};
  std::size_t value_offset{0};

  Instr* left_none_guard{nullptr};
  Instr* right_none_guard{nullptr};
  const YieldValue* left_yield_from{nullptr};
  const YieldValue* value_yield{nullptr};
  const YieldValue* right_yield_from{nullptr};

  // Production capability flags — all false in the experimental first version.
  bool production_admission_artifact_passed{false};
  bool can_exact_reify_yield_from_protocol{false};
  bool can_deopt_state_machine{false};
  bool can_enforce_active_path{false};
  bool can_enforce_depth_budget{false};
};

// TreeIterStateMachinePass converts tree-traversal generators that match the
// canonical in-order pattern:
//
//   def __iter__(self):
//       if self.left is not None:
//           yield from self.left
//       yield self.value
//       if self.right is not None:
//           yield from self.right
//
// into an explicit state machine stored in a heap-backed TreeIterState object
// attached to GenDataFooter.  This eliminates the recursive generator frame
// overhead for each left/right child.
//
// The pass is gated behind the `tree_iter_state_machine` config option
// (env var PYTHONJITTREEITERSTATEMACHINE).  It defaults to disabled and is
// currently an experimental implementation targeting AArch64.
class TreeIterStateMachinePass : public Pass {
 public:
  TreeIterStateMachinePass() : Pass("TreeIterStateMachinePass") {}
  void Run(Function& func) override;

 private:
  // Try to match the canonical TreeIter in-order pattern in func.
  // Returns a populated TreeIterMatch on success, std::nullopt if the
  // function does not match.
  std::optional<TreeIterMatch> matchTreeIter(const Function& func) const;

  // Trace iter_reg back through Phi/Assign/GetIter chains to find the
  // LoadField instruction that produces the iterable.  Returns nullptr if
  // the chain does not end at a LoadField.
  const LoadField* traceYieldFromIterable(const Register* iter_reg) const;

  // Rewrite func's CFG to implement the in-order state machine described by
  // match.  The original recursive yield-from body becomes unreachable and is
  // removed by CleanCFG before this method returns.
  void buildTreeIterStateMachine(
      Function& func,
      const TreeIterMatch& match);
};

} // namespace jit::hir
