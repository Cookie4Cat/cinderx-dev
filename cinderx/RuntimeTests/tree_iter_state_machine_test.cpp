// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/RuntimeTests/fixtures.h"

class TreeIterStateMachineRuntimeTest : public RuntimeTest {};

TEST_F(TreeIterStateMachineRuntimeTest, HelperFailuresPropagate) {
  runStockCode(R"(
import os
import platform
import sys

import cinderx.jit as jit

jit.compile_after_n_calls(1000000)

def state_machine_expected():
    enabled = os.environ.get("PYTHONJITTREEITERSTATEMACHINE", "1").lower()
    arch_supported = platform.machine().lower() in ("aarch64", "arm64")
    return enabled not in ("0", "false", "no") and arch_supported

class Node:
    __slots__ = ("value", "left", "right")

    def __init__(self, value, left=None, right=None):
        self.value = value
        self.left = left
        self.right = right

    def __iter__(self):
        if self.left is not None:
            yield from self.left
        yield self.value
        if self.right is not None:
            yield from self.right

assert jit.force_compile(Node.__iter__)
counts = jit.get_function_hir_opcode_counts(Node.__iter__)
if state_machine_expected():
    assert counts.get("EnsureTreeIterState", 0) > 0, counts

class Other:
    pass

try:
    list(Node(1, left=Other()))
except TypeError as exc:
    assert "TreeIter child type" in str(exc) or "'Other' object is not iterable"
else:
    raise AssertionError("child type mismatch should fail")

if state_machine_expected():
    root = Node(1)
    root.left = root
    old_limit = sys.getrecursionlimit()
    try:
        sys.setrecursionlimit(50)
        try:
            list(root)
        except RecursionError:
            pass
        else:
            raise AssertionError("cycle should raise RecursionError")
    finally:
        sys.setrecursionlimit(old_limit)

class SplitNode:
    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield self.value
        if self.right:
            yield from self.right

assert jit.force_compile(SplitNode.__iter__)
split_root = SplitNode(None, 1, None)
for i in range(50):
    setattr(split_root, f"extra_{i}", i)
assert list(split_root) == [1]
)");
}

TEST_F(TreeIterStateMachineRuntimeTest, GuardMustControlYieldFrom) {
  runStockCode(R"(
import cinderx.jit as jit

jit.compile_after_n_calls(1000000)

class MisplacedGuardNode:
    __slots__ = ("value", "left", "right")

    def __init__(self, value, left=None, right=None):
        self.value = value
        self.left = left
        self.right = right

    def __iter__(self):
        if self.left is not None:
            marker = 1
        yield from self.left
        yield self.value
        if self.right is not None:
            yield from self.right

assert jit.force_compile(MisplacedGuardNode.__iter__)
counts = jit.get_function_hir_opcode_counts(MisplacedGuardNode.__iter__)
assert counts.get("EnsureTreeIterState", 0) == 0, counts

try:
    list(MisplacedGuardNode(42))
except TypeError:
    pass
else:
    raise AssertionError("bare yield-from None must keep raising TypeError")
)");
}
