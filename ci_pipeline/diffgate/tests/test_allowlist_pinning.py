"""Pinning tests for the divergence allowlist.

Every [[rule]] in allowlist.toml must be pinned here with:
  1. a sample pair that differs raw but normalizes equal (the rule fires);
  2. a sample pair with a REAL divergence the rule must NOT wash out.
Run standalone (python3 test_allowlist_pinning.py) or via pytest.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from run_diffgate import ALLOWLIST, load_allowlist, normalize  # noqa: E402

RULES = load_allowlist(ALLOWLIST)
RULE_IDS = [rid for rid, _, _ in RULES]

# rule id -> (raw_a, raw_b, allowed_divergence)
PINS = {
    "mem-addr": [
        # fires: only the object address differs -> allowed
        (
            "CASE x OK <Callee inst at 0x7f83b2c41d90>",
            "CASE x OK <Callee inst at 0xffffa1b2c3d4>",
            True,
        ),
        # must NOT wash out a real value difference next to an address
        (
            "CASE x OK (41, <obj at 0x7f83b2c41d90>)",
            "CASE x OK (42, <obj at 0x7f83b2c41d90>)",
            False,
        ),
        # must NOT touch exception messages without addresses
        (
            "CASE x EXC TypeError: f() takes 2 arguments @f+1",
            "CASE x EXC TypeError: f() takes 3 arguments @f+1",
            False,
        ),
    ],
}


def test_every_rule_is_pinned():
    missing = set(RULE_IDS) - set(PINS)
    assert not missing, "allowlist rules without pinning tests: {}".format(missing)


def test_pins():
    for rid, samples in PINS.items():
        assert rid in RULE_IDS, "pin for unknown rule: {}".format(rid)
        for raw_a, raw_b, allowed in samples:
            equal = normalize(raw_a, RULES) == normalize(raw_b, RULES)
            assert equal == allowed, (
                "rule {}: expected normalized-equal={} for\n  {}\n  {}".format(
                    rid, allowed, raw_a, raw_b
                )
            )


if __name__ == "__main__":
    test_every_rule_is_pinned()
    test_pins()
    print("allowlist pinning: OK ({} rules)".format(len(RULES)))
