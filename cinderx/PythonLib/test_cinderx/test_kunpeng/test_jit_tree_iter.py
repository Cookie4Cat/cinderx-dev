# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import gc
import os
import platform
import sys
import unittest
import weakref

import cinderx
import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.jit import _deopt_gen


_tree_iter_node_counter = 0


def _make_node_class_with_guards():
    """Node class with exact self and field-layout proofs for optimization."""
    global _tree_iter_node_counter
    name = f"_TreeIterNode{_tree_iter_node_counter}"
    _tree_iter_node_counter += 1
    namespace = {}
    exec(
        f"""
class {name}:
    __slots__ = ("value", "left", "right", "__weakref__")

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
""",
        globals(),
        namespace,
    )
    Node = namespace[name]
    globals()[name] = Node
    return Node


def _make_node_class_with_truthiness_guard():
    """Node class whose child guard is default truthiness, with exact proofs."""
    global _tree_iter_node_counter
    name = f"_TreeIterTruthyNode{_tree_iter_node_counter}"
    _tree_iter_node_counter += 1
    namespace = {}
    exec(
        f"""
class {name}:
    __slots__ = ("value", "left", "right", "__weakref__")

    def __init__(self, value, left=None, right=None):
        self.value = value
        self.left = left
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield self.value
        if self.right:
            yield from self.right
""",
        globals(),
        namespace,
    )
    Node = namespace[name]
    globals()[name] = Node
    return Node


def _make_split_dict_node_class_with_truthiness_guard():
    """Pyperformance-style Tree class with split-dict field loads."""
    global _tree_iter_node_counter
    name = f"_TreeIterSplitDictTruthyNode{_tree_iter_node_counter}"
    _tree_iter_node_counter += 1
    namespace = {}
    exec(
        f"""
class {name}:
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
""",
        globals(),
        namespace,
    )
    Node = namespace[name]
    globals()[name] = Node
    return Node


def _build_complete_tree(Node, depth, counter=None):
    """Build a complete binary tree; values are unique integers (BFS order)."""
    if counter is None:
        counter = [0]
    if depth <= 0:
        return None
    left = _build_complete_tree(Node, depth - 1, counter)
    counter[0] += 1
    val = counter[0]
    right = _build_complete_tree(Node, depth - 1, counter)
    return Node(val, left, right)


def _inorder(node):
    """Reference in-order traversal using recursion."""
    if node is None:
        return []
    return _inorder(node.left) + [node.value] + _inorder(node.right)


def _tree_iter_state_machine_enabled():
    value = os.environ.get("PYTHONJITTREEITERSTATEMACHINE", "1")
    return value.lower() not in ("0", "false", "no")


def _tree_iter_state_machine_supported():
    return platform.machine().lower() in ("aarch64", "arm64")


def _tree_iter_state_machine_expected():
    return (
        _tree_iter_state_machine_enabled()
        and _tree_iter_state_machine_supported()
    )


class TreeIterStateMachineTest(unittest.TestCase):
    """Tests for the TreeIter JIT state machine optimization."""

    # ------------------------------------------------------------------
    # Correctness tests — must pass with and without the optimisation
    # ------------------------------------------------------------------

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_pass_fires_only_when_enabled(self):
        """HIR contains TreeIter state-machine ops exactly when the gate is on."""
        Node = _make_node_class_with_guards()
        self.assertTrue(cinderx.jit.force_compile(Node.__iter__))
        ops = cinderx.jit.get_function_hir_opcode_counts(Node.__iter__)

        if _tree_iter_state_machine_expected():
            self.assertGreater(ops.get("EnsureTreeIterState", 0), 0)
            self.assertGreater(ops.get("StateStackPush", 0), 0)
        else:
            self.assertEqual(ops.get("EnsureTreeIterState", 0), 0)
            self.assertEqual(ops.get("StateStackPush", 0), 0)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_depths(self):
        """Depths 1-8: in-order traversal matches recursive reference."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        for depth in range(1, 9):
            root = _build_complete_tree(Node, depth)
            got = list(root)
            want = _inorder(root)
            self.assertEqual(got, want, f"depth={depth}")
            self.assertEqual(len(got), 2**depth - 1)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_repeated_iteration(self):
        """Iterating the same tree twice yields identical results."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 5)
        self.assertEqual(list(root), list(root))

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_guarded_none_children(self):
        """Explicit is-not-None guards: None children are skipped correctly."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        n3 = Node(3)
        n2 = Node(2, right=n3)
        n1 = Node(1, right=n2)
        self.assertEqual(list(n1), [1, 2, 3])

        n3 = Node(3)
        n2 = Node(2, left=n3)
        n1 = Node(1, left=n2)
        self.assertEqual(list(n1), [3, 2, 1])

        self.assertEqual(list(Node(42)), [42])

    # ------------------------------------------------------------------
    # Negative / semantic-boundary tests
    # ------------------------------------------------------------------

    def test_tree_iter_state_machine_bare_yield_from_none_not_optimized(self):
        """Bare ``yield from None`` must raise TypeError, not be silently skipped."""

        class BareYieldFromNoneNode:
            def __init__(self, value):
                self.value = value
                self.left = None
                self.right = None

            def __iter__(self):
                yield from self.left
                yield self.value
                yield from self.right

        if cinderx.jit.is_enabled():
            cinderx.jit.force_compile(BareYieldFromNoneNode.__iter__)

        gen = BareYieldFromNoneNode(42).__iter__()
        with self.assertRaises(TypeError):
            next(gen)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_guard_must_control_yield_from(self):
        """A guard that does not skip yield-from must not admit the state machine."""

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

        self.assertTrue(cinderx.jit.force_compile(MisplacedGuardNode.__iter__))
        ops = cinderx.jit.get_function_hir_opcode_counts(
            MisplacedGuardNode.__iter__
        )
        self.assertEqual(ops.get("EnsureTreeIterState", 0), 0)

        with self.assertRaises(TypeError):
            list(MisplacedGuardNode(42))

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_child_type_mismatch_raises_type_error(self):
        """Runtime child exactness failure must surface the helper exception."""
        Node = _make_node_class_with_guards()

        class Other:
            pass

        self.assertTrue(cinderx.jit.force_compile(Node.__iter__))
        if _tree_iter_state_machine_expected():
            ops = cinderx.jit.get_function_hir_opcode_counts(Node.__iter__)
            self.assertGreater(ops.get("EnsureTreeIterState", 0), 0)

        with self.assertRaises(TypeError):
            list(Node(1, left=Other()))

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_default_path_preserves_non_node_iterables(self):
        """Default-off production path keeps heterogeneous yield-from semantics."""
        if _tree_iter_state_machine_expected():
            self.skipTest("experimental TreeIter path intentionally exact-only")

        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        self.assertEqual(list(Node(1, left=[0], right=(2,))), [0, 1, 2])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_truthiness_guard_default(self):
        """``if child:`` with default truthiness is matched like ``if child is not None:``."""

        Node = _make_node_class_with_truthiness_guard()
        self.assertTrue(cinderx.jit.force_compile(Node.__iter__))

        if _tree_iter_state_machine_expected():
            ops = cinderx.jit.get_function_hir_opcode_counts(Node.__iter__)
            self.assertGreater(
                ops.get("EnsureTreeIterState", 0),
                0,
                "EnsureTreeIterState should appear when truthiness guard is matched",
            )

        root = Node(
            2,
            Node(1),
            Node(3),
        )
        self.assertEqual(list(root), [1, 2, 3])

        big = Node(
            4,
            Node(2, Node(1), Node(3)),
            Node(6, Node(5), Node(7)),
        )
        self.assertEqual(list(big), [1, 2, 3, 4, 5, 6, 7])

        # The state-machine builder must preserve `if child:` semantics and
        # skip non-None falsy children instead of treating them as tree nodes.
        self.assertEqual(list(Node(2, 0, [])), [2])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_split_dict_truthiness_guard(self):
        """Pyperformance's ``generators`` Tree shape uses split-dict fields."""

        Node = _make_split_dict_node_class_with_truthiness_guard()
        self.assertTrue(cinderx.jit.force_compile(Node.__iter__))

        if _tree_iter_state_machine_expected():
            ops = cinderx.jit.get_function_hir_opcode_counts(Node.__iter__)
            self.assertGreater(
                ops.get("EnsureTreeIterState", 0),
                0,
                "EnsureTreeIterState should appear for split-dict Tree nodes",
            )

        root = Node(
            Node(None, 1, None),
            2,
            Node(None, 3, None),
        )
        self.assertEqual(list(root), [1, 2, 3])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_split_dict_combined_dict_fallback(self):
        """Split-dict nodes that become combined dicts must not read stale offsets."""

        Node = _make_split_dict_node_class_with_truthiness_guard()
        self.assertTrue(cinderx.jit.force_compile(Node.__iter__))

        root = Node(None, 1, None)
        for i in range(50):
            setattr(root, f"extra_{i}", i)

        self.assertEqual(list(root), [1])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_custom_bool_not_optimized(self):
        """A node class with custom __bool__ must NOT be matched as a TreeIter."""

        class BoolNode:
            __slots__ = ("value", "left", "right")

            def __init__(self, value, left=None, right=None):
                self.value = value
                self.left = left
                self.right = right

            def __bool__(self):
                return self.value != 0

            def __iter__(self):
                if self.left:
                    yield from self.left
                yield self.value
                if self.right:
                    yield from self.right

        self.assertTrue(cinderx.jit.force_compile(BoolNode.__iter__))
        ops = cinderx.jit.get_function_hir_opcode_counts(BoolNode.__iter__)
        self.assertEqual(
            ops.get("EnsureTreeIterState", 0),
            0,
            "EnsureTreeIterState must be absent when node has custom __bool__",
        )

        root = BoolNode(2, BoolNode(1), BoolNode(3))
        self.assertEqual(list(root), [1, 2, 3])

    def test_tree_iter_state_machine_not_triggered_for_non_tree(self):
        """Plain generators without the left/right/value pattern are unaffected."""

        def simple_gen(n):
            for i in range(n):
                yield i

        if cinderx.jit.is_enabled():
            cinderx.jit.force_compile(simple_gen)

        self.assertEqual(list(simple_gen(5)), list(range(5)))

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_requires_left_value_right_order(self):
        """A right/value/left traversal must not be rewritten as in-order."""

        class ReverseNode:
            def __init__(self, value, left=None, right=None):
                self.value = value
                self.left = left
                self.right = right

            def __iter__(self):
                if self.right is not None:
                    yield from self.right
                yield self.value
                if self.left is not None:
                    yield from self.left

        self.assertTrue(cinderx.jit.force_compile(ReverseNode.__iter__))
        ops = cinderx.jit.get_function_hir_opcode_counts(ReverseNode.__iter__)
        self.assertEqual(ops.get("EnsureTreeIterState", 0), 0)

        root = ReverseNode(2, ReverseNode(1), ReverseNode(3))
        self.assertEqual(list(root), [3, 2, 1])

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_exact_type_required(self):
        """Subclass overriding __iter__ must not be optimised as a TreeIter."""
        Node = _make_node_class_with_guards()

        class SubNode(Node):
            def __iter__(self):
                yield 999
                yield from super().__iter__()

        cinderx.jit.force_compile(Node.__iter__)
        cinderx.jit.force_compile(SubNode.__iter__)

        root = SubNode(1, SubNode(2), SubNode(3))
        result = list(root)
        self.assertEqual(result, [999, 999, 2, 1, 999, 3])

    # ------------------------------------------------------------------
    # State and lifecycle tests
    # ------------------------------------------------------------------

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_completion_clears_state(self):
        """After full iteration, generator is exhausted and GC-safe."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 4)
        gen = root.__iter__()
        results = list(gen)
        self.assertEqual(results, _inorder(root))

        with self.assertRaises(StopIteration):
            next(gen)

        del gen
        gc.collect()

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_deopt_resume_is_fail_closed(self):
        """Explicit deopt must not silently discard the TreeIter heap stack."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 4)
        expected = _inorder(root)
        gen = root.__iter__()
        first = next(gen)

        if _tree_iter_state_machine_expected():
            self.assertFalse(_deopt_gen(gen))
        else:
            self.assertTrue(_deopt_gen(gen))

        self.assertEqual([first, *list(gen)], expected)

        ref = weakref.ref(root)
        del root
        del gen
        gc.collect()
        self.assertIsNone(ref())

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_throw_close_are_fail_closed(self):
        """throw()/close() must not deopt to an incorrect recursive frame."""
        if not _tree_iter_state_machine_expected():
            self.skipTest("TreeIter fail-closed protocol is opt-in only")

        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 3)
        expected = _inorder(root)
        gen = root.__iter__()
        first = next(gen)

        with self.assertRaises(RuntimeError):
            gen.throw(ValueError)
        with self.assertRaises(RuntimeError):
            gen.close()

        self.assertEqual([first, *list(gen)], expected)

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_gc_cycle(self):
        """GC with live generator referencing nodes must not leak or crash."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 4)
        gen = root.__iter__()
        for _ in range(3):
            next(gen)

        ref = weakref.ref(root)
        del root
        gc.collect()
        self.assertIsNotNone(ref())
        del gen
        gc.collect()

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_stack_limit(self):
        """Deep skewed tree exercises heap stack growth without crash."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        depth = 20
        root = None
        for i in range(depth, 0, -1):
            root = Node(i, left=root)

        got = list(root)
        self.assertEqual(got, _inorder(root))

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_cycle_raises_recursion_error(self):
        """Cycles must fail instead of growing the heap stack forever."""
        if not _tree_iter_state_machine_expected():
            self.skipTest(
                "cycle detection is provided by the TreeIter state machine"
            )

        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = Node(1)
        root.left = root

        old_limit = sys.getrecursionlimit()
        try:
            sys.setrecursionlimit(50)
            with self.assertRaises(RecursionError):
                list(root)
        finally:
            sys.setrecursionlimit(old_limit)

        head = Node(0)
        cur = head
        for i in range(1, 140):
            child = Node(i)
            cur.left = child
            cur = child
        cur.left = head
        with self.assertRaises(RecursionError):
            list(head)

    # ------------------------------------------------------------------
    # Protocol gate tests (first-version experiment: fail-closed)
    # ------------------------------------------------------------------

    @cinder_support.skip_unless_jit("Requires CinderX JIT")
    def test_tree_iter_state_machine_protocol_gate(self):
        """send(non-None) on optimised generator must not silently corrupt state."""
        Node = _make_node_class_with_guards()
        cinderx.jit.force_compile(Node.__iter__)

        root = _build_complete_tree(Node, 3)
        gen = root.__iter__()
        first = next(gen)
        self.assertIsNotNone(first)

        val = gen.send(object())
        self.assertEqual([first, val, *list(gen)], _inorder(root))


if __name__ == "__main__":
    unittest.main()
