# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import platform
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest

from cinderx.test_support import ENCODING, SUBPROCESS_TIMEOUT_SEC, subprocess_env


SKIP_PREFIX = "PYTEST_SKIP "


def _expected_elf_machine() -> int:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return 0x3E
    if machine in {"aarch64", "arm64"}:
        return 0xB7
    raise unittest.SkipTest(f"unsupported ELF machine test architecture: {machine}")


def _read_elf_machine(path: Path) -> int:
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF"
    assert data[4] == 2
    assert data[5] == 1
    return struct.unpack_from("<H", data, 18)[0]


def _expected_tool_arch_marker() -> str:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x86-64"
    if machine in {"aarch64", "arm64"}:
        return "aarch64"
    raise unittest.SkipTest(f"unsupported ELF machine test architecture: {machine}")


def _run_external_tool(args: list[str]) -> str | None:
    executable = shutil.which(args[0])
    if executable is None:
        return None

    env = os.environ.copy()
    env["LC_ALL"] = "C"
    completed = subprocess.run(
        [executable, *args[1:]],
        env=env,
        capture_output=True,
        encoding=ENCODING,
        errors="replace",
        timeout=max(SUBPROCESS_TIMEOUT_SEC, 60),
    )
    output = (completed.stdout or "") + (completed.stderr or "")
    assert completed.returncode == 0, (
        f"{args[0]} failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    return output


def _assert_external_tools_report_machine(
    case: unittest.TestCase, path: Path
) -> None:
    expected_marker = _expected_tool_arch_marker()
    for args in (["readelf", "-h", str(path)], ["objdump", "-f", str(path)]):
        if shutil.which(args[0]) is None:
            case.skipTest(f"{args[0]} is not available")

        output = _run_external_tool(args)
        if output is None:
            case.fail(f"{args[0]} is not available")
        normalized_output = output.lower()
        case.assertIn(
            expected_marker,
            normalized_output,
            f"{args[0]} did not report {expected_marker!r} for {path}\n"
            f"output:\n{output}",
        )


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
            raise unittest.SkipTest(line[len(SKIP_PREFIX) :])

    assert completed.returncode == 0, (
        f"subprocess failed with {completed.returncode}\n"
        f"stdout:\n{completed.stdout}\n"
        f"stderr:\n{completed.stderr}"
    )
    assert "Traceback" not in output
    return output


class ElfMachineTests(unittest.TestCase):
    def test_dump_elf_uses_build_machine(self) -> None:
        expected_machine = _expected_elf_machine()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
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

            self.assertEqual(_read_elf_machine(elf_path), expected_machine)
            _assert_external_tools_report_machine(self, elf_path)

    def test_gdb_jit_elf_uses_build_machine(self) -> None:
        expected_machine = _expected_elf_machine()
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
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

            self.assertEqual(_read_elf_machine(elf_path), expected_machine)
            _assert_external_tools_report_machine(self, elf_path)


if __name__ == "__main__":
    unittest.main()
