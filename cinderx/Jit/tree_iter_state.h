// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <cstddef>
#include <cstdint>

namespace jit {

// Each entry on the TreeIter state stack records the node to resume and the
// phase to enter on resumption.
struct TreeIterStackEntry {
  PyObject* node{nullptr};
  int32_t phase{0};
  int32_t reserved{0}; // padding to 16 bytes
};

static_assert(sizeof(TreeIterStackEntry) == 16, "TreeIterStackEntry must be 16 bytes");

// Forward declaration for the non-owning identity set used to detect cycles on
// the active traversal path.
struct TreeIterActivePath;

// Heap-backed state for the TreeIter state machine.  One instance per
// optimised generator, allocated lazily by EnsureTreeIterState at the first
// resume.  The GenDataFooter holds a nullable pointer to this struct so that
// non-TreeIter JIT generators pay no memory cost.
//
// Field naming uses the "tree_iter_" prefix to avoid collisions with any
// future generic generator state fields.
struct TreeIterState {
  // The node currently being processed.
  PyObject* tree_iter_current_node{nullptr};

  // Current traversal phase (TreeIterPhase as int32_t).
  int32_t tree_iter_current_phase{0};

  // Number of valid entries in tree_iter_stack (stack pointer).
  int32_t tree_iter_stack_top{0};

  // Allocated capacity of tree_iter_stack in entries.
  int32_t tree_iter_stack_capacity{0};

  // Current recursion depth (active-path length); production use only.
  int32_t tree_iter_depth{0};

  // Maximum allowed depth derived from the recursion limit at init time.
  int32_t tree_iter_depth_budget{0};

  // Phase of the last-popped stack entry; written by StateStackPop.
  int32_t tree_iter_popped_phase{0};

  int32_t tree_iter_reserved{0}; // padding

  // Growable heap stack.  nullptr until first push.  Owned.
  TreeIterStackEntry* tree_iter_stack{nullptr};

  // Active-path set for cycle detection.  It stores borrowed PyObject*
  // identities; node ownership remains with tree_iter_current_node/stack.
  TreeIterActivePath* tree_iter_active_path{nullptr};
};

// Initial stack capacity in entries.  Covers the main benchmark validation
// depths (up to 15 levels) without requiring a reallocation.
constexpr int32_t kTreeIterInitialStackCapacity = 16;

struct GenDataFooter; // forward declaration

// Allocate and zero-initialise a fresh TreeIterState with an initial stack of
// kTreeIterInitialStackCapacity entries.  Returns nullptr on allocation failure
// without setting a Python exception (the caller raises MemoryError via the
// normal JIT exception path).
TreeIterState* allocateTreeIterState();

// Release all owned PyObject* references in *state and free the object itself.
// Safe to call on nullptr.
void freeTreeIterState(TreeIterState* state);

// Lazily allocate the active-path identity set.  Returns -1 on allocation
// failure without setting a Python exception.
int ensureTreeIterActivePath(TreeIterState* state);

// Shallow trees are faster with a compact stack scan than with an
// unordered_set lookup.  Materialize the hash set only beyond this depth, where
// the scan cost would otherwise dominate skewed trees.
constexpr int32_t kTreeIterActivePathHashThreshold = 128;

// Active-path membership helpers.  The set does not own node references.
int treeIterMaterializeActivePath(TreeIterState* state);
bool treeIterActivePathContains(TreeIterState* state, PyObject* node);
int treeIterActivePathInsert(TreeIterState* state, PyObject* node);
void treeIterActivePathErase(TreeIterState* state, PyObject* node);

// GC traverse: call visit on every PyObject* owned by *state.
// Returns the first non-zero visit result, or 0.
int visitTreeIterState(TreeIterState* state, visitproc visit, void* arg);

// Clear and free any TreeIterState attached to footer.  Idempotent.
void clearTreeIterState(GenDataFooter* footer);

} // namespace jit
