# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import platform
from pathlib import Path
import struct
import subprocess
import sys
import textwrap

import pytest

from cinderx.test_support import ENCODING, SUBPROCESS_TIMEOUT_SEC, subprocess_env


SKIP_PREFIX = "PYTEST_SKIP "


def _expected_elf_machine() -> int:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return 0x3E
    if machine in {"aarch64", "arm64"}:
        return 0xB7
    pytest.skip(f"unsupported ELF machine test architecture: {machine}")


def _read_elf_machine(path: Path) -> int:
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF"
    assert data[4] == 2
    assert data[5] == 1
    return struct.unpack_from("<H", data, 18)[0]


def _subprocess_env(**overrides: str) -> dict[str, str]:
    env = os.environ.copy()
    env.update(subprocess_env())
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE", "PYTHONJITDISABLE"):
        env.pop(name, None)
    env.update(
        {
            "CINDERX_PLUGIN_ENABLE": "1",
            "PYTHONJITALL": "1",
            "PYTHONUNBUFFERED": "1",
        }
    )
    env.update(overrides)
    return env


def _run_child(code: str, tmp_path: Path, env: dict[str, str]) -> str:
    completed = subprocess.run(
        [sys.executable, "-c", code],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        encoding=ENCODING,
        errors="replace",
        timeout=max(SUBPROCESS_TIMEOUT_SEC, 60),
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    for line in output.splitlines():
        if line.startswith(SKIP_PREFIX):
            pytest.skip(line[len(SKIP_PREFIX) :])

    assert completed.returncode == 0, (
        f"subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output
    return output


def test_dump_elf_uses_build_machine(tmp_path: Path) -> None:
    expected_machine = _expected_elf_machine()
    elf_path = tmp_path / "jit_dump.elf"
    code = textwrap.dedent(
        f"""
        from pathlib import Path

        import cinderx
        import cinderx.jit

        try:
            import cinderjit
        except ImportError as exc:
            print({SKIP_PREFIX!r} + f"cinderjit is not available: {{exc}}")
            raise SystemExit(0)

        if not hasattr(cinderjit, "dump_elf"):
            print({SKIP_PREFIX!r} + "cinderjit.dump_elf is not available")
            raise SystemExit(0)

        cinderx.init()
        cinderx.jit.enable()
        if not cinderx.jit.is_enabled():
            print({SKIP_PREFIX!r} + "CinderX JIT is not enabled")
            raise SystemExit(0)

        def dump_elf_machine_target(value):
            return value + 1

        if not cinderx.jit.force_compile(dump_elf_machine_target):
            print({SKIP_PREFIX!r} + "force_compile returned False")
            raise SystemExit(0)

        assert dump_elf_machine_target(41) == 42
        cinderjit.dump_elf({str(elf_path)!r})
        assert Path({str(elf_path)!r}).is_file()
        """
    )

    _run_child(code, tmp_path, _subprocess_env())

    assert _read_elf_machine(elf_path) == expected_machine


def test_gdb_jit_elf_uses_build_machine(tmp_path: Path) -> None:
    expected_machine = _expected_elf_machine()
    elf_path = tmp_path / "gdb_jit.elf"
    code = textwrap.dedent(
        f"""
        from pathlib import Path
        import shutil

        import cinderx
        import cinderx.jit

        tmp_dir = Path("/tmp")
        pattern = "cinder_PyFunctionObject_*_elf"
        before = {{path.resolve() for path in tmp_dir.glob(pattern)}}

        cinderx.init()
        cinderx.jit.enable()
        if not cinderx.jit.is_enabled():
            print({SKIP_PREFIX!r} + "CinderX JIT is not enabled")
            raise SystemExit(0)

        def gdb_jit_elf_machine_target(value):
            return value * 3

        if not cinderx.jit.force_compile(gdb_jit_elf_machine_target):
            print({SKIP_PREFIX!r} + "force_compile returned False")
            raise SystemExit(0)

        assert gdb_jit_elf_machine_target(14) == 42
        after = {{path.resolve() for path in tmp_dir.glob(pattern)}}
        matches = sorted(
            after - before,
            key=lambda path: path.stat().st_mtime_ns,
        )
        assert matches, f"no GDB JIT ELF files matched {{pattern!r}}"

        shutil.copyfile(matches[-1], {str(elf_path)!r})
        for generated_path in matches:
            generated_path.unlink(missing_ok=True)
        """
    )

    _run_child(
        code,
        tmp_path,
        _subprocess_env(PYTHONJITGDBWRITEELF="1"),
    )

    assert _read_elf_machine(elf_path) == expected_machine
