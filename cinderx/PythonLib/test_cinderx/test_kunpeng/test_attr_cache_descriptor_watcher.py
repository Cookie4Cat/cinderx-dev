# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

import cinderx.jit
from cinderx.test_support import failUnlessJITCompiled, passIf


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
class AttrCacheDescriptorWatcherTests(unittest.TestCase):
    def test_shared_descr_type_across_cache_entries(self) -> None:
        class Descr:
            def __get__(self, obj, ty):
                return "descr"

            def __set__(self, obj, val):
                raise RuntimeError("unimplemented")

        class T1:
            foo = Descr()

        class T2:
            foo = Descr()

        @failUnlessJITCompiled
        def get_attr(obj):
            return obj.foo

        t1 = T1()
        t2 = T2()

        # Prime and cache both receiver-type entries with the same descriptor
        # type in one polymorphic attribute cache.
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t2), "descr")
        self.assertEqual(get_attr(t2), "descr")

        t1.__dict__["foo"] = "t1 attr"
        t2.__dict__["foo"] = "t2 attr"
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t2), "descr")

        # Mutating T1 invalidates only T1's cache entry. T2 must keep watching
        # Descr so deleting __set__ below invalidates T2's data-descriptor IC.
        T1.bar = 1
        del Descr.__set__

        self.assertEqual(get_attr(t2), "t2 attr")


if __name__ == "__main__":
    unittest.main()
