# Copyright (c) Meta Platforms, Inc. and affiliates.

import pytest

import cinderx.jit


class Body:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


def dist_sq(a, b):
    dx = a.x - b.x
    dy = a.y - b.y
    dz = a.z - b.z
    return dx * dx + dy * dy + dz * dz


def _specialize_then_compile(func, runner):
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)
    try:
        # Keep these calls interpreted so CPython quickens the bytecode shape
        # before we force the JIT compiler to observe it.
        for _ in range(10000):
            runner()
    finally:
        cinderx.jit.jit_unsuppress(func)

    assert cinderx.jit.force_compile(func)


@pytest.fixture(autouse=True)
def _specialized_opcodes():
    if not cinderx.jit.is_enabled():
        pytest.skip("requires CinderX JIT")

    cinderx.jit.enable_specialized_opcodes()
    yield
    cinderx.jit.disable_specialized_opcodes()


def test_primitive_box_remat_elides_frame_state_only_boxes():
    p = Body(1.0, 2.0, 3.0)
    q = Body(4.0, 5.0, 6.0)
    _specialize_then_compile(dist_sq, lambda: dist_sq(p, q))

    counts = cinderx.jit.get_function_hir_opcode_counts(dist_sq)
    assert counts is not None
    assert counts.get("DoubleBinaryOp", 0) >= 5, counts
    load_attr_count = counts.get("LoadAttr", 0) + counts.get("LoadAttrCached", 0)
    assert load_attr_count >= 6, counts
    assert counts.get("PrimitiveBox", 0) == 1, counts
    assert dist_sq(p, q) == 27.0


def test_primitive_box_remat_deopt_reconstructs_double_locals():
    class IntYBody:
        x = 7.0
        y = 5
        z = 11.0

    p = Body(1.0, 2.0, 3.0)
    q = Body(4.0, 5.0, 6.0)
    _specialize_then_compile(dist_sq, lambda: dist_sq(p, q))

    cinderx.jit.clear_runtime_stats()
    assert dist_sq(p, IntYBody()) == 109.0
    stats = cinderx.jit.get_and_clear_runtime_stats()
    assert len(stats.get("deopt") or ()) >= 1, stats
