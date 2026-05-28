// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/tree_iter_state.h"

#include "cinderx/Jit/gen_data_footer.h"

#include <cstdlib>
#include <cstring>

namespace jit {

TreeIterState* allocateTreeIterState() {
  auto* state = static_cast<TreeIterState*>(
      std::malloc(sizeof(TreeIterState)));
  if (state == nullptr) {
    return nullptr;
  }
  std::memset(state, 0, sizeof(TreeIterState));

  auto* stack = static_cast<TreeIterStackEntry*>(
      std::calloc(kTreeIterInitialStackCapacity, sizeof(TreeIterStackEntry)));
  if (stack == nullptr) {
    std::free(state);
    return nullptr;
  }
  state->tree_iter_stack = stack;
  state->tree_iter_stack_capacity = kTreeIterInitialStackCapacity;
  return state;
}

void freeTreeIterState(TreeIterState* state) {
  if (state == nullptr) {
    return;
  }
  // Release current node.
  Py_XDECREF(state->tree_iter_current_node);
  state->tree_iter_current_node = nullptr;

  // Release all owned stack entries.
  const int32_t top = state->tree_iter_stack_top;
  if (state->tree_iter_stack != nullptr) {
    for (int32_t i = 0; i < top; i++) {
      Py_XDECREF(state->tree_iter_stack[i].node);
      state->tree_iter_stack[i].node = nullptr;
    }
    std::free(state->tree_iter_stack);
    state->tree_iter_stack = nullptr;
  }

  // Production active-path is not yet implemented; nothing to free.
  // state->tree_iter_active_path is always nullptr in this version.

  std::free(state);
}

int visitTreeIterState(TreeIterState* state, visitproc visit, void* arg) {
  if (state == nullptr) {
    return 0;
  }

  // Visit current node.
  Py_VISIT(state->tree_iter_current_node);

  // Visit all owned stack entries.
  const int32_t top = state->tree_iter_stack_top;
  if (state->tree_iter_stack != nullptr) {
    for (int32_t i = 0; i < top; i++) {
      Py_VISIT(state->tree_iter_stack[i].node);
    }
  }

  return 0;
}

void clearTreeIterState(GenDataFooter* footer) {
  if (footer == nullptr) {
    return;
  }
  freeTreeIterState(footer->tree_iter_state);
  footer->tree_iter_state = nullptr;
}

} // namespace jit
