import argparse

import cinderx.jit as jit


class Box:
    pass


class Other:
    pass


class Descriptor:
    def __get__(self, obj, typ=None):
        if obj is None:
            return self
        return obj.seed + 5


class WithDescriptor:
    value = Descriptor()


class SlotBox:
    __slots__ = ("value",)


def read(obj):
    return obj.value


def cached_fastpath() -> None:
    jit.enable()
    jit.enable_specialized_opcodes()
    jit.compile_after_n_calls(1000000)

    box = Box()
    box.value = 42

    for _ in range(20000):
        read(box)

    assert jit.force_compile(read)
    counts = jit.get_function_hir_opcode_counts(read)
    assert counts.get("LoadAttrCached", 0) >= 1, counts
    assert read(box) == 42

    other = Other()
    other.value = 43
    assert read(other) == 43

    slot = SlotBox()
    slot.value = 44
    assert read(slot) == 44

    descr = WithDescriptor()
    descr.seed = 45
    assert read(descr) == 50

    try:
        read(Other())
    except AttributeError:
        pass
    else:
        raise AssertionError("missing attribute should raise AttributeError")

    def custom_getattribute(self, name):
        if name == "value":
            return 99
        return object.__getattribute__(self, name)

    Box.__getattribute__ = custom_getattribute
    assert read(box) == 99


def managed_dict_without_inline_values() -> None:
    jit.enable()
    jit.enable_specialized_opcodes()
    jit.compile_after_n_calls(1000000)

    class EncodingBytes(bytes):
        def __new__(cls, value):
            return bytes.__new__(cls, value)

        def __init__(self, value):
            self._position = 0

        def match_bytes(self, needle):
            return self.startswith(needle, self._position)

    buf = EncodingBytes(b"abcdef")
    assert jit.force_compile(EncodingBytes.match_bytes)
    assert buf.match_bytes(b"abc") is True


CASES = {
    "cached-fastpath": cached_fastpath,
    "managed-dict-without-inline-values": managed_dict_without_inline_values,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
