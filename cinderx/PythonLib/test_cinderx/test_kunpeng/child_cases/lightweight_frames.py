import sys

import cinderx
import cinderx.jit

try:
    import cinderjit
except ImportError:
    cinderjit = None


def minimal_jit_target() -> int:
    return 41 + 1


def run_case(force_fallback: bool = False) -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")

    if force_fallback:
        cinderjit._test_set_thread_state_offset(-1)

    assert cinderx.jit.force_compile(minimal_jit_target)
    assert cinderx.jit.is_jit_compiled(minimal_jit_target)
    for _ in range(20):
        assert minimal_jit_target() == 42
    print("CASE_RESULT minimal_jit_target OK 42")
    cinderx.jit.disassemble(minimal_jit_target)


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in {"fallback", "inline"}:
        raise SystemExit(f"usage: {sys.argv[0]} <fallback|inline>")
    run_case(force_fallback=sys.argv[1] == "fallback")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
