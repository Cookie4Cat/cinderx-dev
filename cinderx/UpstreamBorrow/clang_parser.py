# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Parse CPython sources using libclang and a compilation database."""

from __future__ import annotations

import ctypes.util
import json
import os
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import clang
from clang.cindex import Config, Diagnostic, Index, TranslationUnit


ArgFilter = Callable[[list[str]], list[str]]


def setup_libclang_dll() -> None:
    """Locate libclang from an override, the PyPI wheel, or the OS loader."""

    if (
        Config.loaded
        or Config.library_file is not None
        or Config.library_path is not None
    ):
        return

    override = os.environ.get("LIBCLANG_PATH")
    if override:
        path = Path(override).expanduser().resolve()
        if path.is_dir():
            Config.set_library_path(str(path))
        elif path.is_file():
            Config.set_library_file(str(path))
        else:
            raise RuntimeError(f"LIBCLANG_PATH does not exist: {path}")
        return

    native = Path(clang.__file__).resolve().parent / "native"
    for name in ("libclang.so", "libclang.dylib", "libclang.dll"):
        candidate = native / name
        if candidate.is_file():
            Config.set_library_file(str(candidate))
            return

    library = ctypes.util.find_library("clang")
    if library:
        Config.set_library_file(library)


def filter_cpp_args(args: list[str]) -> list[str]:
    """Remove compiler-driver arguments that libclang cannot consume."""

    result: list[str] = []
    skip_next = False
    source_suffixes = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")
    paired_driver_args = {"-o", "-MF", "-MT", "-MQ", "-MJ"}

    for index, arg in enumerate(args):
        if skip_next:
            skip_next = False
            continue
        if index == 0 and not arg.startswith("-"):
            # Compiler executable from compile_commands.json.
            continue
        if arg in paired_driver_args:
            skip_next = True
            continue
        if arg in {"-c", "-MD", "-MMD", "-MP"}:
            continue
        if arg == "--":
            continue
        if not arg.startswith("-") and arg.lower().endswith(source_suffixes):
            continue
        if arg.startswith(("-Wp,-M", "-fdiagnostics-color")):
            continue
        result.append(arg)
    return result


@dataclass(frozen=True)
class _CompileCommand:
    directory: Path
    file: Path
    arguments: list[str]


class ClangParser:
    """Parse source files using a standard ``compile_commands.json``."""

    def __init__(
        self,
        compile_commands: str,
        arg_filter: ArgFilter,
    ) -> None:
        database = Path(compile_commands).resolve()
        if database.is_dir():
            database /= "compile_commands.json"
        if not database.is_file():
            raise FileNotFoundError(f"compilation database not found: {database}")

        raw_commands = json.loads(database.read_text(encoding="utf-8"))
        if not isinstance(raw_commands, list) or not raw_commands:
            raise RuntimeError(f"empty compilation database: {database}")

        self.arg_filter = arg_filter
        self.commands: list[_CompileCommand] = []
        for raw in raw_commands:
            directory = Path(raw["directory"]).resolve()
            file = Path(raw["file"])
            if not file.is_absolute():
                file = directory / file
            if "arguments" in raw:
                arguments = list(raw["arguments"])
            else:
                arguments = shlex.split(raw["command"])
            self.commands.append(
                _CompileCommand(directory, file.resolve(), arguments)
            )

        self.root = self._find_source_root(database.parent)

    def _find_source_root(self, fallback: Path) -> Path:
        candidates = [command.file.parent for command in self.commands]
        candidates.extend(command.directory for command in self.commands)
        candidates.append(fallback)
        for candidate in candidates:
            for parent in (candidate, *candidate.parents):
                if (parent / "Include" / "Python.h").is_file():
                    return parent
        return fallback

    @staticmethod
    def _suffix_matches(path: Path, source_file: str) -> bool:
        normalized_path = path.as_posix()
        normalized_source = source_file.replace("\\", "/")
        return normalized_path == normalized_source or normalized_path.endswith(
            "/" + normalized_source
        )

    def path_to_file(self, source_file: str) -> str:
        for command in self.commands:
            if self._suffix_matches(command.file, source_file):
                return str(command.file)
        candidate = (self.root / source_file).resolve()
        if candidate.is_file():
            return str(candidate)
        raise FileNotFoundError(
            f"cannot resolve {source_file!r} from CPython root {self.root}"
        )

    def _command_for(self, source_file: str) -> _CompileCommand:
        for command in self.commands:
            if self._suffix_matches(command.file, source_file):
                return command
        # Headers do not normally have their own compilation-database entry.
        return self.commands[0]

    def chdir_to_root(self) -> None:
        os.chdir(self.root)

    def parse(self, source_file: str) -> TranslationUnit:
        source_path = self.path_to_file(source_file)
        command = self._command_for(source_file)
        args = self.arg_filter(command.arguments)

        previous_cwd = Path.cwd()
        try:
            os.chdir(command.directory)
            translation_unit = Index.create().parse(source_path, args=args)
        finally:
            os.chdir(previous_cwd)

        # GCC's resource headers can produce recoverable semantic diagnostics
        # when parsed by libclang (notably around C11 atomics). The AST remains
        # usable and TemplateFileProcessor independently requires every
        # non-optional declaration. Fatal diagnostics, such as a missing
        # header, make the translation unit unreliable and must stop generation.
        fatal_errors = [
            diagnostic
            for diagnostic in translation_unit.diagnostics
            if diagnostic.severity >= Diagnostic.Fatal
        ]
        if fatal_errors:
            rendered = "\n".join(
                f"  {diagnostic}" for diagnostic in fatal_errors
            )
            raise RuntimeError(
                f"libclang could not parse {source_path}:\n{rendered}"
            )
        return translation_unit


def arg_filter(args: list[str]) -> list[str]:
    new_args = filter_cpp_args(args)
    # Borrow needs declarations that CPython guards out of non-debug builds.
    return [arg for arg in new_args if arg != "-DNDEBUG"]


class ParsedFile:
    source_file: str
    translation_unit: TranslationUnit
    lines: list[str]

    def __init__(self, source_file: str, translation_unit: TranslationUnit) -> None:
        self.source_file = source_file
        self.translation_unit = translation_unit
        with open(source_file, "r") as f:
            self.lines = f.read().split("\n")


class FileParser:
    def __init__(self, compile_commands: str) -> None:
        self.parser = ClangParser(compile_commands, arg_filter=arg_filter)
        self.parsed_files: dict[str, ParsedFile] = {}
        setup_libclang_dll()

    def chdir_to_root(self) -> None:
        self.parser.chdir_to_root()

    def parse(self, source_file: str) -> ParsedFile:
        if self.parsed_files.get(source_file) is None:
            tu = self.parser.parse(source_file)
            source_path = self.parser.path_to_file(source_file)
            self.parsed_files[source_file] = ParsedFile(source_path, tu)
        return self.parsed_files[source_file]
