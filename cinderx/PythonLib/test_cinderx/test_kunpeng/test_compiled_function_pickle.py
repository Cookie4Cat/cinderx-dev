# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Regression test for https://gitcode.com/openeuler/cinderx/issues/17.

The JIT pins a function's compiled machine code by storing a
CompiledFunction handle in the function's __dict__ under
``__cinderx_compiled_func__``. The pin has to live there so the GC can
see it and collect module-teardown cycles, but machine code is
process-local, so the handle used to make previously picklable function
state (``pickle.dumps(func.__dict__)``) raise TypeError after JIT
compilation -- including for functions the user never compiled
explicitly: once a code object has a compiled artifact, brand-new
functions created from it get the handle attached on creation.
CompiledFunction now reduces to a None placeholder: pickling and
deepcopy keep working, restoring needs nothing from cinderx, and the
live function stays compiled throughout.

Each test uses its own factory (its own code object) so that the
JIT's reattach-on-creation behavior cannot couple the tests together.
"""

import copy
import pickle
import sys
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf

COMPILED_KEY = "__cinderx_compiled_func__"


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class CompiledFunctionPickleTests(unittest.TestCase):
    def test_function_dict_stays_picklable_after_compile(self) -> None:
        def target_pickle(value):
            return value + 1

        self.assertEqual(pickle.loads(pickle.dumps(target_pickle.__dict__)), {})

        self.assertTrue(cinderx.jit.force_compile(target_pickle))
        # The GC-visible pin is intentionally still in place.
        self.assertIn(COMPILED_KEY, target_pickle.__dict__)

        # This used to raise "TypeError: cannot pickle 'CompiledFunction'
        # object" for every protocol.
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            with self.subTest(protocol=proto):
                data = pickle.dumps(target_pickle.__dict__, protocol=proto)
                self.assertEqual(pickle.loads(data), {COMPILED_KEY: None})

        # Pickling must not disturb the live function.
        self.assertTrue(cinderx.jit.is_jit_compiled(target_pickle))
        self.assertEqual(target_pickle(41), 42)

    def test_user_attributes_survive_the_round_trip(self) -> None:
        def target_attrs(value):
            return value + 2

        target_attrs.marker = "issue-17"
        self.assertTrue(cinderx.jit.force_compile(target_attrs))

        restored = pickle.loads(pickle.dumps(target_attrs.__dict__))
        self.assertEqual(restored["marker"], "issue-17")
        self.assertIsNone(restored[COMPILED_KEY])

    def test_reduce_reconstructor_is_picklable_by_reference(self) -> None:
        # Mirrors the upstream regression test from 369dd1ab, which lives in
        # test_cinderjit.py and cannot execute in wheel-based environments
        # (its module imports need the xxclassloader test extension).
        def target_reduce(value):
            return value + 4

        self.assertTrue(cinderx.jit.force_compile(target_reduce))
        compiled = target_reduce.__dict__[COMPILED_KEY]

        reconstructor, args = compiled.__reduce__()
        self.assertEqual(args, ())
        self.assertIsNone(reconstructor())
        # Picklable by reference (a module-level function): this is what lets
        # cloudpickle drop the artifact when serializing a function's __dict__
        # by value.
        self.assertIs(
            getattr(
                sys.modules[reconstructor.__module__], reconstructor.__qualname__
            ),
            reconstructor,
        )
        # End-to-end: the handle itself round-trips to None.
        self.assertIsNone(pickle.loads(pickle.dumps(compiled)))

    def test_function_dict_deepcopy_after_compile(self) -> None:
        def target_deepcopy(value):
            return value + 3

        self.assertTrue(cinderx.jit.force_compile(target_deepcopy))

        # copy.deepcopy goes through the same reduce protocol.
        dup = copy.deepcopy(target_deepcopy.__dict__)
        self.assertEqual(dup, {COMPILED_KEY: None})
        self.assertTrue(cinderx.jit.is_jit_compiled(target_deepcopy))
        self.assertEqual(target_deepcopy(39), 42)


if __name__ == "__main__":
    unittest.main()
