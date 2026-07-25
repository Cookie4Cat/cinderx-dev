# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Generate or verify cached UpstreamBorrow output without Meta Buck."""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


_BORROW_DECL = re.compile(
    r"// @Borrow (?:optional )?(?:function|typedef|var|struct|enum) "
    r"\S+ from (\S+)"
)
_BORROW_CPP = re.compile(
    r".*// @Borrow CPP directives(?: noinclude)? from (\S+)"
)
_VERSION_DEFINE = re.compile(
    r"^#define\s+PY_(MAJOR|MINOR|MICRO)_VERSION\s+(\d+)\s*$"
)
_STANDALONE_CONFIG = Path(__file__).with_name("standalone_config.h").resolve()


@dataclass(frozen=True)
class PythonProbe:
    executable: Path
    version: tuple[int, int, int]
    include_dirs: tuple[Path, ...]
    cc: str
    gil_disabled: bool


def collect_borrow_sources(template: Path) -> list[str]:
    """Return CPython source paths in first-use order from a template."""

    result: list[str] = []
    seen: set[str] = set()
    for line in template.read_text(encoding="utf-8").splitlines():
        match = _BORROW_CPP.match(line) or _BORROW_DECL.match(line)
        if match and match.group(1) not in seen:
            source = match.group(1)
            result.append(source)
            seen.add(source)
    if not result:
        raise RuntimeError(f"no @Borrow directives found in {template}")
    return result


def read_cpython_version(source_root: Path) -> tuple[int, int, int]:
    """Read the exact CPython version from ``Include/patchlevel.h``."""

    patchlevel = source_root / "Include" / "patchlevel.h"
    if not patchlevel.is_file():
        raise FileNotFoundError(f"not a CPython source tree: {source_root}")
    values: dict[str, int] = {}
    for line in patchlevel.read_text(encoding="utf-8").splitlines():
        if match := _VERSION_DEFINE.match(line):
            values[match.group(1)] = int(match.group(2))
    missing = {"MAJOR", "MINOR", "MICRO"} - values.keys()
    if missing:
        raise RuntimeError(
            f"could not read {', '.join(sorted(missing))} from {patchlevel}"
        )
    return values["MAJOR"], values["MINOR"], values["MICRO"]


def probe_python(executable: Path) -> PythonProbe:
    """Read version and build configuration from the target interpreter."""

    if not executable.is_file():
        raise FileNotFoundError(f"target Python not found: {executable}")

    script = r"""
import json
import sys
import sysconfig

paths = sysconfig.get_paths()
print(json.dumps({
    "executable": sys.executable,
    "version": list(sys.version_info[:3]),
    "include": paths.get("include"),
    "platinclude": paths.get("platinclude"),
    "cc": sysconfig.get_config_var("CC") or "cc",
    "gil_disabled": bool(sysconfig.get_config_var("Py_GIL_DISABLED")),
}))
"""
    try:
        completed = subprocess.run(
            [str(executable), "-c", script],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError(
            f"could not probe target Python {executable}: {error.stderr.strip()}"
        ) from error
    try:
        data = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"target Python did not return sysconfig JSON: {completed.stdout!r}"
        ) from error

    include_dirs: list[Path] = []
    for value in (data.get("include"), data.get("platinclude")):
        if value:
            path = Path(value).resolve()
            if path not in include_dirs:
                include_dirs.append(path)
    return PythonProbe(
        executable=Path(data["executable"]).resolve(),
        version=tuple(data["version"]),
        include_dirs=tuple(include_dirs),
        cc=data["cc"],
        gil_disabled=data["gil_disabled"],
    )


def validate_versions(
    source_version: tuple[int, int, int],
    python_version: tuple[int, int, int],
    requested_version: str | None,
    allow_patch_mismatch: bool,
) -> str:
    """Validate private-API provenance and return the major.minor label."""

    if source_version[:2] != python_version[:2]:
        raise RuntimeError(
            "CPython source/interpreter major.minor mismatch: "
            f"{source_version[0]}.{source_version[1]} vs "
            f"{python_version[0]}.{python_version[1]}"
        )
    if source_version != python_version and not allow_patch_mismatch:
        raise RuntimeError(
            "CPython source/interpreter patch mismatch: "
            f"{'.'.join(map(str, source_version))} vs "
            f"{'.'.join(map(str, python_version))}; "
            "pass --allow-patch-mismatch only for an intentional port"
        )

    version = requested_version or f"{python_version[0]}.{python_version[1]}"
    expected = f"{source_version[0]}.{source_version[1]}"
    if version != expected:
        raise RuntimeError(
            f"--version {version} does not match CPython source {expected}"
        )
    return version


def compiler_builtin_include_dirs(cc: str) -> tuple[Path, ...]:
    """Ask GCC/Clang for resource headers not discovered by libclang wheels."""

    command = shlex.split(cc) if cc else ["cc"]
    if not command or shutil.which(command[0]) is None:
        return ()

    candidates: list[Path] = []
    probes = (
        ["-print-file-name=include"],
        ["-print-file-name=include-fixed"],
        ["-print-resource-dir"],
    )
    for probe in probes:
        try:
            completed = subprocess.run(
                [*command, *probe],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
        except (OSError, subprocess.CalledProcessError):
            continue
        value = completed.stdout.strip()
        if not value or value in {"include", "include-fixed"}:
            continue
        path = Path(value)
        if probe == ["-print-resource-dir"]:
            path /= "include"
        path = path.resolve()
        if path.is_dir() and path not in candidates:
            candidates.append(path)
    return tuple(candidates)


def build_compile_database(
    destination: Path,
    source_root: Path,
    build_root: Path,
    sources: Sequence[str],
    probe: PythonProbe,
    extra_defines: Sequence[str] = (),
    extra_include_dirs: Sequence[Path] = (),
) -> None:
    """Create the minimal standard compilation database needed by libclang."""

    if not build_root.is_dir():
        raise FileNotFoundError(f"CPython build directory not found: {build_root}")

    compiler = shlex.split(probe.cc)[0] if probe.cc else "cc"
    builtin_include_dirs = compiler_builtin_include_dirs(probe.cc)
    include_dirs = [
        build_root,
        source_root / "Include" / "internal",
        source_root / "Include" / "internal" / "mimalloc",
        source_root,
        source_root / "Include",
        *probe.include_dirs,
        *(path.resolve() for path in extra_include_dirs),
    ]
    unique_include_dirs: list[Path] = []
    for include_dir in include_dirs:
        include_dir = include_dir.resolve()
        if include_dir not in unique_include_dirs and include_dir.is_dir():
            unique_include_dirs.append(include_dir)

    if not any((path / "pyconfig.h").is_file() for path in unique_include_dirs):
        raise RuntimeError(
            "pyconfig.h was not found in the CPython build or interpreter "
            "include directories"
        )

    common_args = [
        compiler,
        "-std=gnu11",
        "-ferror-limit=0",
        "-DPy_BUILD_CORE",
        *(["-DPy_GIL_DISABLED"] if probe.gil_disabled else []),
        *(f"-D{define}" for define in extra_defines),
        *(f"-I{path}" for path in unique_include_dirs),
        *(f"-isystem{path}" for path in builtin_include_dirs),
        "-include",
        str(_STANDALONE_CONFIG),
    ]
    commands = []
    for source in sources:
        source_path = (source_root / source).resolve()
        try:
            source_path.relative_to(source_root)
        except ValueError as error:
            raise RuntimeError(
                f"borrow source escapes CPython tree: {source}"
            ) from error
        if not source_path.is_file():
            raise FileNotFoundError(f"borrow source not found: {source_path}")
        commands.append(
            {
                "directory": str(build_root),
                "file": str(source_path),
                "arguments": [*common_args, "-c", str(source_path)],
            }
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        json.dumps(commands, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def borrow_sources_sha256(source_root: Path, sources: Sequence[str]) -> str:
    """Hash the ordered Borrow source set, including relative path names."""

    digest = hashlib.sha256()
    for source in sources:
        digest.update(source.encode("utf-8"))
        digest.update(b"\0")
        digest.update((source_root / source).read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _print_diff(expected: Path, actual: Path, limit: int) -> None:
    expected_lines = expected.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines()
    actual_lines = actual.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines()
    diff = difflib.unified_diff(
        expected_lines,
        actual_lines,
        fromfile=str(expected),
        tofile=str(actual),
        lineterm="",
    )
    for index, line in enumerate(diff):
        if index >= limit:
            print(f"... diff truncated after {limit} lines", file=sys.stderr)
            break
        print(line, file=sys.stderr)


def _write_atomically(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    file_descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    os.close(file_descriptor)
    temporary = Path(temporary_name)
    try:
        temporary.write_bytes(source.read_bytes())
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def _parse_args(argv: Sequence[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate or verify CinderX UpstreamBorrow output using a public "
            "CPython source tree and libclang."
        )
    )
    parser.add_argument("--cpython-source", type=Path, required=True)
    parser.add_argument(
        "--cpython-build",
        type=Path,
        help="configured CPython build tree; defaults to --cpython-source",
    )
    parser.add_argument(
        "--python",
        type=Path,
        default=Path(sys.executable),
        help="exact target interpreter used to obtain pyconfig.h and version",
    )
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", help="major.minor label; inferred by default")
    parser.add_argument(
        "--compile-commands",
        type=Path,
        help="use an existing standard compile_commands.json",
    )
    parser.add_argument(
        "--emit-compile-commands",
        type=Path,
        help="save the effective compilation database for provenance",
    )
    parser.add_argument(
        "--define",
        action="append",
        default=[],
        help="additional preprocessor definition, without -D",
    )
    parser.add_argument(
        "--include-dir",
        action="append",
        default=[],
        type=Path,
        help="additional include directory",
    )
    parser.add_argument(
        "--allow-patch-mismatch",
        action="store_true",
        help="allow source/interpreter micro versions to differ",
    )
    parser.add_argument(
        "--diff-lines",
        type=int,
        default=200,
        help="maximum unified-diff lines printed by --check",
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--check",
        action="store_true",
        help="verify that generated bytes match the existing --output",
    )
    mode.add_argument(
        "--write",
        action="store_true",
        help="atomically replace --output with generated bytes",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(argv)
    source_root = args.cpython_source.resolve()
    build_root = (args.cpython_build or source_root).resolve()
    template = args.template.resolve()
    output = args.output.resolve()
    python = args.python.resolve()

    source_version = read_cpython_version(source_root)
    probe = probe_python(python)
    version = validate_versions(
        source_version,
        probe.version,
        args.version,
        args.allow_patch_mismatch,
    )
    sources = collect_borrow_sources(template)

    print(f"Target Python: {probe.executable}")
    print(f"Target version: {'.'.join(map(str, probe.version))}")
    print(f"CPython source: {source_root}")
    print(f"Borrow template: {template}")
    print(f"Template sha256: {sha256(template)}")
    print(f"Borrow sources: {len(sources)}")
    print(f"Borrow sources sha256: {borrow_sources_sha256(source_root, sources)}")

    with tempfile.TemporaryDirectory(prefix="cinderx-borrow-") as directory:
        temporary_root = Path(directory)
        generated = temporary_root / output.name
        if args.compile_commands:
            compile_commands = args.compile_commands.resolve()
            if not compile_commands.is_file():
                raise FileNotFoundError(
                    f"compilation database not found: {compile_commands}"
                )
        else:
            compile_commands = temporary_root / "compile_commands.json"
            build_compile_database(
                compile_commands,
                source_root,
                build_root,
                sources,
                probe,
                args.define,
                args.include_dir,
            )

        if args.emit_compile_commands:
            emitted = args.emit_compile_commands.resolve()
            emitted.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(compile_commands, emitted)
            print(f"Compilation database: {emitted}")
        else:
            print(f"Compilation database: {compile_commands}")

        # Import only for real generation so helper tests do not require
        # libclang or Click.
        from .UpstreamBorrow import TemplateFileProcessor
        from .clang_parser import FileParser

        processor = TemplateFileProcessor(
            str(template),
            str(generated),
            FileParser(str(compile_commands)),
            version,
        )
        processor.process()

        generated_hash = sha256(generated)
        if args.check:
            if not output.is_file():
                raise FileNotFoundError(f"checked output not found: {output}")
            expected_hash = sha256(output)
            if generated.read_bytes() != output.read_bytes():
                print(
                    "Borrow cache mismatch: "
                    f"expected sha256={expected_hash}, "
                    f"generated sha256={generated_hash}",
                    file=sys.stderr,
                )
                _print_diff(output, generated, args.diff_lines)
                return 1
            print(f"CHECK OK: byte-identical sha256={generated_hash}")
        else:
            _write_atomically(generated, output)
            print(f"WROTE: {output} sha256={generated_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
