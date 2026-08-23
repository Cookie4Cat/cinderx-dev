"""Selftests for the quickened-counter artifact judgement.

The judgement decides whether a regrtest -R "memory blocks" line may be
dismissed.  Getting it wrong in the permissive direction hides a real
leak, so the cases below pin both directions.
"""

import unittest

from ci_pipeline.jit311.quickened_artifact import decide


class DecideTests(unittest.TestCase):
    def test_flat_blocks_with_matching_counter_drift_is_an_artifact(self):
        artifact, why = decide(
            "test_listcomps",
            [8, 8, 8, 8],
            [1000, 1000, 1000, 1000, 1000],
            [-10, -18, -26, -34, -42],
        )
        self.assertTrue(artifact, why)
        self.assertIn("artifact", why)

    def test_growing_raw_blocks_is_a_real_leak(self):
        artifact, why = decide(
            "test_x", [8, 8, 8], [1000, 1008, 1016, 1024], [-10, -18, -26, -34]
        )
        self.assertFalse(artifact, why)
        self.assertIn("real leak", why)

    def test_counter_that_does_not_drift_cannot_excuse_a_figure(self):
        artifact, why = decide(
            "test_x", [8, 8, 8], [1000, 1000, 1000, 1000], [5, 5, 5, 5]
        )
        self.assertFalse(artifact, why)
        self.assertIn("real leak", why)

    def test_counter_drift_of_the_wrong_size_is_unexplained(self):
        # Counter falls by one per repetition; eight blocks were reported.
        artifact, why = decide(
            "test_x", [8, 8, 8], [1000, 1000, 1000, 1000], [-1, -2, -3, -4]
        )
        self.assertFalse(artifact, why)
        self.assertIn("unexplained", why)

    def test_early_compilation_growth_does_not_reach_the_verdict(self):
        # Blocks climb steeply while the JIT is still compiling, then
        # settle.  Judging the whole window would call this a leak; only
        # the settled tail counts.
        artifact, why = decide(
            "test_listcomps",
            [8, 8, 8, 8],
            [1000, 1300, 1600, 1600, 1600, 1600, 1600],
            [-10, -18, -26, -34, -42, -50, -58],
        )
        self.assertTrue(artifact, why)

    def test_blocks_that_shrink_are_still_acceptable(self):
        artifact, why = decide(
            "test_x", [3, 3, 3], [1000, 999, 998, 997], [-3, -6, -9, -12]
        )
        self.assertTrue(artifact, why)


if __name__ == "__main__":
    unittest.main()
