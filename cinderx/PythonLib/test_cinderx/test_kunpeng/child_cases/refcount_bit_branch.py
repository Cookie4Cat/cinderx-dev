import sys

import cinderx.jit


def refcount_target(value: object) -> object:
    values = [value, value]
    wrapped = {"values": values}
    return wrapped["values"][0]


def run_case() -> None:
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if not cinderx.jit.force_compile(refcount_target):
        raise RuntimeError("could not compile refcount target")

    marker = object()
    before = sys.getrefcount(marker)
    for _ in range(1000):
        if refcount_target(marker) is not marker:
            raise AssertionError("JIT result did not preserve object identity")
    after = sys.getrefcount(marker)
    if after != before:
        raise AssertionError(f"refcount changed: before={before}, after={after}")

    immortal_before = sys.getrefcount(None)
    for _ in range(1000):
        if refcount_target(None) is not None:
            raise AssertionError("JIT result did not preserve immortal singleton")
    immortal_after = sys.getrefcount(None)
    if immortal_after != immortal_before:
        raise AssertionError(
            "immortal refcount changed: "
            f"before={immortal_before}, after={immortal_after}"
        )

    print("CASE_RESULT refcount_bit_branch OK")
    cinderx.jit.disassemble(refcount_target)


if __name__ == "__main__":
    run_case()
