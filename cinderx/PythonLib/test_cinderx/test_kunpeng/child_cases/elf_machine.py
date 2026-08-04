from pathlib import Path
import shutil
import sys

import cinderx
import cinderx.jit


SKIP_PREFIX = "PYTEST_SKIP "


def skip(reason: str) -> None:
    print(SKIP_PREFIX + reason)
    raise SystemExit(0)


def initialize_jit() -> None:
    cinderx.init()
    cinderx.jit.enable()
    if not cinderx.jit.is_enabled():
        skip("CinderX JIT is not enabled")


def dump_elf(output: Path) -> None:
    try:
        import cinderjit
    except ImportError as exc:
        skip(f"cinderjit is not available: {exc}")

    if not hasattr(cinderjit, "dump_elf"):
        skip("cinderjit.dump_elf is not available")

    initialize_jit()

    def dump_elf_machine_target(value):
        return value + 1

    if not cinderx.jit.force_compile(dump_elf_machine_target):
        skip("force_compile returned False")

    assert dump_elf_machine_target(41) == 42
    cinderjit.dump_elf(str(output))
    assert output.is_file()


def gdb_jit_elf(output: Path) -> None:
    tmp_dir = Path("/tmp")
    pattern = "cinder_PyFunctionObject_*_elf"
    before = {path.resolve() for path in tmp_dir.glob(pattern)}

    initialize_jit()

    def gdb_jit_elf_machine_target(value):
        return value * 3

    if not cinderx.jit.force_compile(gdb_jit_elf_machine_target):
        skip("force_compile returned False")

    assert gdb_jit_elf_machine_target(14) == 42
    after = {path.resolve() for path in tmp_dir.glob(pattern)}
    matches = sorted(
        after - before,
        key=lambda path: path.stat().st_mtime_ns,
    )
    assert matches, f"no GDB JIT ELF files matched {pattern!r}"

    shutil.copyfile(matches[-1], output)
    for generated_path in matches:
        generated_path.unlink(missing_ok=True)


CASES = {
    "dump-elf": dump_elf,
    "gdb-jit-elf": gdb_jit_elf,
}


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in CASES:
        choices = ", ".join(sorted(CASES))
        raise SystemExit(f"usage: {sys.argv[0]} <{choices}> <output>")
    CASES[sys.argv[1]](Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
