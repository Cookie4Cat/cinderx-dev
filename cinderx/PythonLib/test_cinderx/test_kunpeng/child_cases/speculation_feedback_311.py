# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Child programs for test_speculation_feedback_311 (CPython 3.11, execute
mode).  Each case prints ``CASE_OK <case>`` when every assertion held."""

import array
import gc
import math
import sys

import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit  # noqa: E402

assert cinderx.is_initialized() and cinderjit.is_enabled()
assert _cinderx.is_frame_evaluator_installed()

# Enough interpreted calls for the 3.11 adaptive interpreter to specialise.
WARMUP_CALLS = 20

ARITHMETIC = """
def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def div(a, b):
    return a / b

def iadd(a, b):
    a += b
    return a

def isub(a, b):
    a -= b
    return a

def imul(a, b):
    a *= b
    return a

def idiv(a, b):
    a /= b
    return a

def lt(a, b):
    return a < b

def le_branch(a, b):
    if a <= b:
        return 1
    return 2

def eq_branch(a, b):
    if a == b:
        return 1
    return 2

def ne(a, b):
    return a != b
"""

ACCUMULATE = """
def acc(xs):
    s = 0
    for x in xs:
        s += x
    return s
"""


class FloatSub(float):
    """A float subclass whose operators must never take a float fast path."""

    def __add__(self, other):
        return ("sub-add", float(self), other)

    __radd__ = __add__

    def __lt__(self, other):
        return "sub-lt"


SPECIAL_OPERANDS = [
    0.0,
    -0.0,
    1.5,
    -2.25,
    math.inf,
    -math.inf,
    math.nan,
    1e308,
    5e-324,
    3,
    0,
    -7,
    True,
    2**70,
    2**1100,
    FloatSub(2.5),
]


def build(source, tag):
    namespace = {}
    exec(compile(source, f"<speculation-{tag}>", "exec"), namespace)
    return namespace


def outcome(func, *args):
    try:
        return ("value", func(*args))
    except Exception as exc:  # the exception type is part of the contract
        return ("raise", type(exc))


def same(expected, actual):
    if expected[0] != actual[0]:
        return False
    if expected[0] == "raise":
        return expected[1] is actual[1]
    a, b = expected[1], actual[1]
    if type(a) is not type(b):
        return False
    if isinstance(a, float):
        if math.isnan(a):
            return math.isnan(b)
        return a == b and math.copysign(1.0, a) == math.copysign(1.0, b)
    return a == b


def compile_now(func):
    assert cinderjit.force_compile(func), func
    assert cinderjit.is_jit_compiled(func), func


def machine_code_entries():
    return _cinderx._get_trigger_stats()["machine_code_entries"]


def organic_deopts():
    return _cinderx._get_trigger_stats()["organic_deopt_hits"]


def case_forced():
    """Forced deopts are not evidence of bad speculation."""
    for literal, argument, result in ((1.0, 2.5, 3.5), (1, 2, 3)):
        ns = build(f"def target(x):\n    return x + {literal!r}\n", "forced")
        target = ns["target"]
        for _ in range(100):
            assert target(argument) == result
        compile_now(target)
        sites = [s for s in cinderjit.deopt_sites(target) if s["forceable"]]
        assert sites, cinderjit.deopt_sites(target)
        site = max(sites, key=lambda s: s["bc_offset"])
        before = _cinderx._get_trigger_stats()
        for i in range(32):
            assert cinderjit.force_deopt(target, site["id"])
            assert target(argument) == result
            assert cinderjit.is_jit_compiled(target), i
        after = _cinderx._get_trigger_stats()
        assert after["forced_deopt_hits"] - before["forced_deopt_hits"] == 32
        assert after["organic_deopt_hits"] == before["organic_deopt_hits"]
        assert after["machine_code_entries"] > before["machine_code_entries"]


def case_semantics():
    """Speculated float arithmetic and compares match the interpreter on
    signed zeros, infinities, NaN, overflow, zero division, ints, bools and
    float subclasses, whether the site was specialised (float warm-up: guards,
    fuse, diamond after recompile) or stuck (mixed warm-up: diamond)."""
    oracle = build(ARITHMETIC, "oracle")
    warmups = {
        "float": [(1.5, 2.25), (-0.5, 4.0)],
        "mixed": [(1, 2.5), (2.5, 1), (1.5, 2.0), (3, 4)],
    }
    names = [n for n in oracle if not n.startswith("__")]
    entries = machine_code_entries()
    for warm_name, warm_args in warmups.items():
        compiled = build(ARITHMETIC, f"jit-{warm_name}")
        for name in names:
            func = compiled[name]
            for i in range(WARMUP_CALLS):
                func(*warm_args[i % len(warm_args)])
            compile_now(func)
        for name in names:
            func = compiled[name]
            for a in SPECIAL_OPERANDS:
                for b in SPECIAL_OPERANDS:
                    if not cinderjit.is_jit_compiled(func):
                        # A blown fuse uncompiles; keep testing machine code.
                        compile_now(func)
                    expected = outcome(oracle[name], a, b)
                    actual = outcome(func, a, b)
                    assert same(expected, actual), (
                        warm_name, name, a, b, expected, actual)
    assert machine_code_entries() > entries

    oracle_acc = build(ACCUMULATE, "acc-oracle")["acc"]
    acc = build(ACCUMULATE, "acc-jit")["acc"]
    for _ in range(WARMUP_CALLS):
        acc([1.0, 2.5])
    compile_now(acc)
    for xs in ([], [1.0, 2.5], [1, 2.0], [2.0, 1], [-0.0], [0.0, -0.0],
               [math.inf, -math.inf], [math.nan], [True, 0.5], [2**1100, 1.0],
               [FloatSub(1.0), 1.0], [1.0, FloatSub(1.0)]):
        if not cinderjit.is_jit_compiled(acc):
            compile_now(acc)
        expected = outcome(oracle_acc, xs)
        actual = outcome(acc, xs)
        assert same(expected, actual), (xs, expected, actual)


def case_fuse():
    """A *_FLOAT guard that keeps failing at one site is recompiled into a
    type-check diamond there: eight organic deopts, then none."""
    ns = build("def scale(a, b):\n    return a * b\n", "fuse")
    scale = ns["scale"]
    for _ in range(WARMUP_CALLS):
        assert scale(1.5, 2.0) == 3.0
    compile_now(scale)
    before = organic_deopts()
    for _ in range(8):
        assert scale(3, 4) == 12
    assert organic_deopts() - before == 8, organic_deopts() - before
    if not cinderjit.is_jit_compiled(scale):
        compile_now(scale)
    after_fuse = organic_deopts()
    entries = machine_code_entries()
    for _ in range(100):
        assert scale(3, 4) == 12
        assert scale(1.5, 2.0) == 3.0
    assert organic_deopts() == after_fuse, organic_deopts() - after_fuse
    assert machine_code_entries() - entries == 200


FUSE_ARTIFACTS = """
def make():
    def scale(a, b):
        return a * b
    return scale
"""


def deopts_while(calls):
    before = organic_deopts()
    for call in calls:
        call()
    return organic_deopts() - before


def case_fuse_by_code():
    """A fuse that blows in a by-code artifact (fresh function objects over
    one code, compiled through the observer) retires that artifact: the
    deopts stop after the eighth."""
    ns = build(FUSE_ARTIFACTS, "fuse-by-code")
    make = ns["make"]
    entries = machine_code_entries()
    for _ in range(200):
        assert make()(1.5, 2.0) == 3.0
    assert machine_code_entries() > entries
    assert deopts_while([lambda: make()(3, 4)] * 8) == 8
    assert deopts_while([lambda: make()(3, 4)] * 12) == 0
    assert make()(1.5, 2.0) == 3.0


EXC_EDGE = """
def guarded(b):
    try:
        return 2.5 * b
    except TypeError:
        return -1
"""


def case_exc_edge():
    """A float site inside a try block (one operand known float, so it
    would get a type-check diamond) keeps its generic form once the exception
    edge is granted, so the compiled handler takes the error without a
    deopt."""
    guarded = build(EXC_EDGE, "exc-edge")["guarded"]
    compile_now(guarded)
    # Eight exception deopts earn the edge and recompile the code in place.
    assert deopts_while([lambda: guarded("x")] * 8) == 8
    assert cinderjit.is_jit_compiled(guarded)
    entries = machine_code_entries()
    assert deopts_while([lambda: guarded("x")] * 12) == 0
    assert machine_code_entries() - entries == 12
    assert guarded("x") == -1
    assert guarded(2.0) == 5.0


IDENTITY = """
def binary(x, rhs):
    a = x * 2.0
    return a, a {op} rhs

def inplace(x, rhs):
    a = x * 2.0
    b = a
    b {op}= rhs
    return a, b

def read_after(x, rhs):
    a = x * 2.0
    b = a {op} rhs
    return a is b
"""


class Reflect:
    """Reflected operators hand back the float operand itself."""

    def __radd__(self, other):
        return other

    __rsub__ = __rmul__ = __rtruediv__ = __radd__


def case_identity():
    """A boxed float that is still read elsewhere reaches the generic slow arm
    as the same object, not an equal copy."""
    rhs = Reflect()
    entries = machine_code_entries()
    for op in "+-*/":
        ns = build(IDENTITY.format(op=op), f"identity-{op}")
        for name in ("binary", "inplace", "read_after"):
            func = ns[name]
            for _ in range(WARMUP_CALLS):
                func(12.5, rhs)
            compile_now(func)
            result = func(12.5, rhs)
            if name == "read_after":
                assert result is True, (op, name)
            else:
                a, b = result
                assert a == 25.0 and a is b, (op, name, a, b)
    assert machine_code_entries() > entries


def case_lifetime():
    """Fuse state is created by concurrent compile workers and dropped with
    its code object."""
    def count():
        return cinderjit._jit311_lifecycle_snapshot()["jit"]["numeric_spec_codes"]

    def collect():
        for _ in range(3):
            gc.collect()

    baseline = count()
    for cycle in range(2):
        units = []
        for i in range(256):
            ns = build("def unit(a, b):\n    return a * b\n", f"unit-{cycle}-{i}")
            fn = ns["unit"]
            for _ in range(WARMUP_CALLS):
                fn(1.5, 2.0)
            assert cinderjit._jit311_register_for_compile(fn)
            units.append(fn)
        cinderjit._jit311_multithreaded_compile_test()
        assert all(cinderjit.is_jit_compiled(fn) for fn in units)
        assert count() == baseline + len(units), (count(), baseline)
        for fn in units:
            assert fn(1.5, 2.0) == 3.0
        for fn in units:
            assert cinderjit.force_uncompile(fn)
        del fn, ns, units
        collect()
        assert count() == baseline, (count(), baseline)


INT_ARITHMETIC = """
def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def iadd(a, b):
    a += b
    return a

def isub(a, b):
    a -= b
    return a

def imul(a, b):
    a *= b
    return a

def floordiv_pow2(a, b):
    return a // 2, a // 1024

def mod_pow2(a, b):
    return a % 8, a % 1024

def band(a, b):
    return a & b

def bor(a, b):
    return a | b

def bxor(a, b):
    return a ^ b

def iand(a, b):
    a &= b
    return a

def ior(a, b):
    a |= b
    return a

def ixor(a, b):
    a ^= b
    return a

def masked(a, b):
    return ((a >> 3) & 0xFF) ^ b

def truthy(a, b):
    if a & b:
        return 1
    return 2

def band_box(a, b):
    return (a + 1) & (b - 1)

def bor_box(a, b):
    return (a * 3) | b

def bxor_const(a, b):
    return (a ^ 0x2AAAAAAA) ^ b

def iand_box(a, b):
    a += 1
    a &= b
    return a

def truthy_const(a, b):
    if a & 0x40:
        return 1
    return 2
"""

INT_SHIFTS = """
def lshift(a, n):
    return a << n

def rshift(a, n):
    return a >> n

def ilshift(a, n):
    a <<= n
    return a

def irshift(a, n):
    a >>= n
    return a

def lshift_const(a, n):
    return a << 5, a << 63

def rshift_const(a, n):
    return a >> 5, a >> 63

def guarded_shift(a, n):
    try:
        return a << n
    except ValueError:
        return "negative"

def lshift_box(a, n):
    return (a + 1) << n

def rshift_box(a, n):
    return (a - 1) >> n
"""


class IntSub(int):
    """An int subclass whose operators must never take an int fast path."""

    def __and__(self, other):
        return ("sub-and", int(self), other)

    __rand__ = __and__

    def __lshift__(self, other):
        return ("sub-lshift", int(self), other)

    def __add__(self, other):
        return ("sub-add", int(self), other)

    __radd__ = __add__


class AndProtocol:
    """A user type reached through the generic number protocol."""

    def __and__(self, other):
        return "and-protocol"

    __rand__ = __and__


# Digit boundaries of the 30-bit representation and of int64.
INT_OPERANDS = [
    0,
    1,
    -1,
    7,
    -7,
    2**29,
    2**30 - 1,
    -(2**30 - 1),
    2**30,
    -(2**30),
    2**59,
    -(2**59),
    2**60 - 1,
    -(2**60 - 1),
    2**60,
    -(2**60),
    2**63 - 1,
    -(2**63),
    2**64,
    -(2**100),
    True,
    False,
    IntSub(6),
    1.5,
    frozenset({1, 2}),
    AndProtocol(),
]

SHIFT_COUNTS = [0, 1, 5, 29, 30, 31, 59, 60, 62, 63, 64, 65, 100, -1, -64,
                True, IntSub(3), 1.5]


def check_against_oracle(source, tag, warmups, second_operands):
    oracle = build(source, f"{tag}-oracle")
    names = [n for n in oracle if not n.startswith("__")]
    entries = machine_code_entries()
    for warm_name, warm_args in warmups.items():
        compiled = build(source, f"{tag}-jit-{warm_name}")
        for name in names:
            func = compiled[name]
            for i in range(WARMUP_CALLS):
                # Mixed warm-up operands raise for the bitwise operators.
                outcome(func, *warm_args[i % len(warm_args)])
            compile_now(func)
        for name in names:
            func = compiled[name]
            for a in INT_OPERANDS:
                for b in second_operands:
                    if not cinderjit.is_jit_compiled(func):
                        # A blown fuse uncompiles; keep testing machine code.
                        compile_now(func)
                    expected = outcome(oracle[name], a, b)
                    actual = outcome(func, a, b)
                    assert same(expected, actual), (
                        warm_name, name, a, b, expected, actual)
    assert machine_code_entries() > entries


def case_int_semantics():
    """Unboxed int arithmetic, floor division and modulo by powers of two,
    bitwise operators and shifts match the interpreter across digit and int64
    boundaries, negatives, overflow into arbitrary precision, bools, int
    subclasses, the user number protocol and raising operands."""
    warmups = {
        "int": [(3, 5), (7, 2), (-4, 9)],
        "mixed": [(3, 5), (1.5, 2), (2**70, 3), (True, 1)],
    }
    check_against_oracle(INT_ARITHMETIC, "int", warmups, INT_OPERANDS)
    warmups = {"int": [(3, 2), (-5, 7), (255, 1)]}
    check_against_oracle(INT_SHIFTS, "shift", warmups, SHIFT_COUNTS)


def case_int_fuse():
    """An unboxed int operation deopts on an operand it cannot handle, and a
    site that keeps doing so is recompiled with the generic operation."""
    for source, good, good_result, bad, result in (
        ("def op(a, b):\n    return a & 0xFF\n",
         (6, 3), 6, (2**70 + 5, 0), 5),
        ("def op(a, b):\n    return a + b\n",
         (6, 3), 9, (2**40, 1), 2**40 + 1),
    ):
        op = build(source, "int-fuse")["op"]
        for _ in range(WARMUP_CALLS):
            op(*good)
        compile_now(op)
        before = organic_deopts()
        for _ in range(8):
            assert op(*bad) == result
        assert organic_deopts() - before == 8, (source, organic_deopts() - before)
        if not cinderjit.is_jit_compiled(op):
            compile_now(op)
        after_fuse = organic_deopts()
        entries = machine_code_entries()
        for _ in range(100):
            assert op(*bad) == result
            assert op(*good) == good_result
        assert organic_deopts() == after_fuse, (source, organic_deopts() - after_fuse)
        assert machine_code_entries() - entries >= 100

    # A constant that does not fit in int64 leaves the operation generic: an
    # operand of any type runs without deoptimizing.
    big = build("def big(a):\n    return a & 0x10000000000000000000\n", "int-big")["big"]
    for _ in range(WARMUP_CALLS):
        big(5)
    compile_now(big)
    before = organic_deopts()
    for _ in range(20):
        assert big(AndProtocol()) == "and-protocol"
        assert big(2**70 + 1) == 2**70 & 0x10000000000000000000
    assert organic_deopts() == before, organic_deopts() - before


def case_int_wide_operands():
    """Ints of unknown origin may be arbitrarily wide, so an operator with no
    small-int operand stays generic: 128-bit masks never deoptimize."""
    source = """
def masked(address, mask):
    packed = int(address)
    return (packed & int(mask)) != packed

class Net:
    def __init__(self, ip):
        self._ip = ip

    def within(self, mask):
        return self._ip & mask, self._ip ^ mask
"""
    ns = build(source, "int-wide")
    masked, net = ns["masked"], ns["Net"](2**100 + 7)
    for _ in range(WARMUP_CALLS):
        masked(3, 1)
        net.within(1)
    compile_now(masked)
    compile_now(ns["Net"].within)
    before = organic_deopts()
    entries = machine_code_entries()
    ip = net._ip
    for address, mask in ((2**100, 2**128 - 1), (2**100 + 1, 2**100),
                          (-2**100, -1), (2**32 - 1, 2**32 - 1), (5, 4)):
        assert masked(address, mask) == ((address & mask) != address)
        assert net.within(mask) == (ip & mask, ip ^ mask), mask
    assert organic_deopts() == before, organic_deopts() - before
    assert machine_code_entries() - entries == 10


def case_int_slow_arm_identity():
    """An int kept on registers that also reaches the generic arm of an
    array.array store is passed there as the same object a later deopt
    rebuilds the local from: `is` keeps its interpreter result."""
    source = """
def store(c, i, n, z):
    j = i * n + 1
    c[j] = 0
    return j + z
"""

    class Rec:
        key = None

        def __setitem__(self, key, value):
            Rec.key = key

    class Probe:
        def __radd__(self, other):
            return other is Rec.key

    store = build(source, "int-identity")["store"]
    arr = array.array("l", [0] * 4000)
    for _ in range(WARMUP_CALLS):
        store(arr, 30, 30, 1)
    compile_now(store)
    entries = machine_code_entries()
    assert store(Rec(), 30, 30, Probe()) is True
    assert machine_code_entries() - entries == 1


def case_int_deopt_values():
    """A deopt after unboxed intermediates rebuilds them exactly: the boxes
    the JIT never allocated are recreated from the register values."""
    source = """
def chain(a, b, c):
    x = (a << 4) | b
    y = x ^ c
    return x, y

def index(i, n, c):
    j = i * n + 1
    return j, j + c
"""
    oracle = build(source, "remat-oracle")
    compiled = build(source, "remat-jit")
    for name, warm in (("chain", (3, 1, 2)), ("index", (3, 4, 5))):
        func = compiled[name]
        for _ in range(WARMUP_CALLS):
            func(*warm)
        compile_now(func)
        for args in ((3, 1, 2**100), (3, 1, IntSub(2)), (3, 1, 1.5),
                     (2**58, 3, 2**100), (2**59, 3, 1), (-(2**58), -1, 2**100),
                     (2**29, 2**29, 2**100), (-(2**29), 2**29, 1.5),
                     (2**30, 2, 1)):
            if not cinderjit.is_jit_compiled(func):
                compile_now(func)
            expected = outcome(oracle[name], *args)
            actual = outcome(func, *args)
            assert same(expected, actual), (name, args, expected, actual)


CASES = {
    "forced": case_forced,
    "semantics": case_semantics,
    "fuse": case_fuse,
    "fuse_by_code": case_fuse_by_code,
    "exc_edge": case_exc_edge,
    "identity": case_identity,
    "lifetime": case_lifetime,
    "int_semantics": case_int_semantics,
    "int_fuse": case_int_fuse,
    "int_deopt_values": case_int_deopt_values,
    "int_wide_operands": case_int_wide_operands,
    "int_slow_arm_identity": case_int_slow_arm_identity,
}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in CASES:
        raise SystemExit(f"usage: {sys.argv[0]} {{{'|'.join(CASES)}}}")
    CASES[sys.argv[1]]()
    print(f"CASE_OK {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
