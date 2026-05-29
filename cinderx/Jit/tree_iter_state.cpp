// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/tree_iter_state.h"

#include "cinderx/Jit/gen_data_footer.h"

#include <cstdlib>
#include <new>
#include <unordered_set>

namespace jit {

struct TreeIterActivePath {
  std::unordered_set<PyObject*> nodes;
};

TreeIterState* allocateTreeIterState() {
  auto* state = new (std::nothrow) TreeIterState();
  if (state == nullptr) {
    return nullptr;
  }

  auto* stack = static_cast<TreeIterStackEntry*>(
      std::calloc(kTreeIterInitialStackCapacity, sizeof(TreeIterStackEntry)));
  if (stack == nullptr) {
    delete state;
    return nullptr;
  }
  state->tree_iter_stack = stack;
  state->tree_iter_stack_capacity = kTreeIterInitialStackCapacity;
  return state;
}

int ensureTreeIterActivePath(TreeIterState* state) {
  if (state == nullptr) {
    return -1;
  }
  if (state->tree_iter_active_path != nullptr) {
    return 0;
  }
  auto* active_path = new (std::nothrow) TreeIterActivePath();
  if (active_path == nullptr) {
    return -1;
  }
  state->tree_iter_active_path = active_path;
  return 0;
}

bool treeIterActivePathContains(TreeIterState* state, PyObject* node) {
  if (state == nullptr || state->tree_iter_active_path == nullptr ||
      node == nullptr) {
    return false;
  }
  return state->tree_iter_active_path->nodes.count(node) != 0;
}

int treeIterActivePathInsert(TreeIterState* state, PyObject* node) {
  if (node == nullptr) {
    return 0;
  }
  if (ensureTreeIterActivePath(state) < 0) {
    return -1;
  }
  try {
    state->tree_iter_active_path->nodes.insert(node);
  } catch (const std::bad_alloc&) {
    return -1;
  }
  return 0;
}

void treeIterActivePathErase(TreeIterState* state, PyObject* node) {
  if (state == nullptr || state->tree_iter_active_path == nullptr ||
      node == nullptr) {
    return;
  }
  state->tree_iter_active_path->nodes.erase(node);
}

void freeTreeIterState(TreeIterState* state) {
  if (state == nullptr) {
    return;
  }
  PyObject* exc_type = nullptr;
  PyObject* exc_value = nullptr;
  PyObject* exc_tb = nullptr;
  PyErr_Fetch(&exc_type, &exc_value, &exc_tb);

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

  delete state->tree_iter_active_path;
  state->tree_iter_active_path = nullptr;

  delete state;

  if (exc_type != nullptr || exc_value != nullptr || exc_tb != nullptr) {
    PyErr_Restore(exc_type, exc_value, exc_tb);
  } else {
    PyErr_Clear();
  }
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
