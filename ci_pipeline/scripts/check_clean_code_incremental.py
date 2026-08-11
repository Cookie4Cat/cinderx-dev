#!/usr/bin/env python3
"""Check clean-code rules for changed files, or for the full tracked tree.

The repository-level .clang-format/.clang-tidy files define the style, but this
script defaults to the diff against a base ref so that introducing clean-code
config does not require reformatting historical files. Use --all for a full
tracked-tree baseline scan.
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import difflib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from typing import Iterable, TextIO


LLVM_MAJOR_VERSION = "12"

C_FORMAT_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
}

TIDY_SOURCE_EXTENSIONS = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
}


def timestamp() -> str:
    return _datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")

EXCLUDED_PREFIXES = (
    "ThirdParty/",
    "cinderx/ThirdParty/",
    "cinderx/Interpreter/3.11/upstream/",
    "cinderx/UpstreamBorrow/",
)

EXCLUDED_PARTS = (
    "/generated_",
    ".gen.",
)


def run(cmd: list[str], *, cwd: Path, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=cwd,
        check=check,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def print_output(result: subprocess.CompletedProcess[str]) -> None:
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)


def run_logged(cmd: list[str], *, cwd: Path) -> subprocess.CompletedProcess[str]:
    print("Running:", " ".join(cmd))
    result = run(cmd, cwd=cwd, check=False)
    print_output(result)
    return result


def write_report(report: TextIO | None, text: str) -> None:
    if report is not None:
        report.write(text)
        report.flush()


def default_format_report_path(repo: Path) -> Path:
    return repo / "build" / "clean-code" / timestamp() / "logs" / "clang-format.diff"


def print_format_diff(
    path: str,
    *,
    cwd: Path,
    clang_format: str,
    line_args: list[str],
    report: TextIO | None,
) -> None:
    original = (cwd / path).read_text(errors="replace").splitlines(keepends=True)
    result = run([clang_format, *line_args, path], cwd=cwd, check=False)
    if result.returncode != 0:
        print_output(result)
        write_report(report, result.stdout)
        write_report(report, result.stderr)
        return

    formatted = result.stdout.splitlines(keepends=True)
    diff = "".join(
        difflib.unified_diff(
        original,
        formatted,
        fromfile=f"a/{path}",
        tofile=f"b/{path}",
        )
    )
    print("Suggested clang-format diff:")
    print(diff, end="")
    write_report(report, "Suggested clang-format diff:\n")
    write_report(report, diff)
    if diff and not diff.endswith("\n"):
        print()
        write_report(report, "\n")


def default_llvm_tool(tool: str, env_var: str) -> str:
    override = os.environ.get(env_var)
    if override:
        return override
    versioned = f"{tool}-{LLVM_MAJOR_VERSION}"
    if shutil.which(versioned):
        return versioned
    return tool


def check_llvm_tool_version(tool: str, *, display_name: str) -> int:
    if shutil.which(tool) is None:
        print(f"error: {tool!r} not found; install LLVM/Clang {LLVM_MAJOR_VERSION} or set {display_name}", file=sys.stderr)
        return 2

    result = subprocess.run(
        [tool, "--version"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    version_text = result.stdout or result.stderr
    if f"version {LLVM_MAJOR_VERSION}." not in version_text and f"version {LLVM_MAJOR_VERSION} " not in version_text:
        first_line = version_text.splitlines()[0] if version_text.splitlines() else "unknown version"
        print(
            f"error: {tool!r} must be LLVM/Clang {LLVM_MAJOR_VERSION}.x for clean-code checks; got: {first_line}",
            file=sys.stderr,
        )
        return 2
    return 0


def git_lines(args: list[str], *, cwd: Path) -> list[str]:
    result = run(["git", *args], cwd=cwd)
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def git_text(args: list[str], *, cwd: Path) -> str:
    return run(["git", *args], cwd=cwd).stdout


def is_candidate(path: str) -> bool:
    normalized = path.replace("\\", "/")
    if normalized.startswith(EXCLUDED_PREFIXES):
        return False
    if normalized.startswith("cinderx/Interpreter/") and "/Includes/" in normalized:
        return False
    if any(part in normalized for part in EXCLUDED_PARTS):
        return False
    return Path(normalized).suffix in C_FORMAT_EXTENSIONS


def changed_files(cwd: Path, base: str, include_untracked: bool) -> list[str]:
    files: set[str] = set()

    # Diff committed/staged/unstaged tracked changes against the chosen base.
    files.update(git_lines(["diff", "--name-only", "--diff-filter=ACMRT", f"{base}...HEAD"], cwd=cwd))
    files.update(git_lines(["diff", "--name-only", "--diff-filter=ACMRT"], cwd=cwd))
    files.update(git_lines(["diff", "--cached", "--name-only", "--diff-filter=ACMRT"], cwd=cwd))

    if include_untracked:
        files.update(git_lines(["ls-files", "--others", "--exclude-standard"], cwd=cwd))

    return candidate_files(cwd, files)


def parse_changed_ranges(diff_text: str) -> dict[str, list[tuple[int, int]]]:
    ranges: dict[str, list[tuple[int, int]]] = {}
    path: str | None = None

    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            target = line[4:]
            path = None if target == "/dev/null" else target.removeprefix("b/")
            continue
        if path is None or not line.startswith("@@ "):
            continue

        header = line.split("@@", 2)[1].strip()
        new_range = next((part for part in header.split() if part.startswith("+")), None)
        if new_range is None:
            continue
        start_text, _, count_text = new_range[1:].partition(",")
        start = int(start_text)
        count = int(count_text) if count_text else 1
        if count > 0:
            ranges.setdefault(path, []).append((start, start + count - 1))
    return ranges


def merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if not merged or start > merged[-1][1] + 1:
            merged.append((start, end))
        else:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
    return merged


def line_count(path: Path) -> int:
    with path.open("rb") as file:
        count = sum(1 for _ in file)
    return max(count, 1)


def changed_file_ranges(cwd: Path, base: str, include_untracked: bool) -> dict[str, list[tuple[int, int]]]:
    ranges: dict[str, list[tuple[int, int]]] = {}

    for args in (
        ["diff", "--unified=0", "--diff-filter=ACMRT", f"{base}...HEAD"],
        ["diff", "--unified=0", "--diff-filter=ACMRT"],
        ["diff", "--cached", "--unified=0", "--diff-filter=ACMRT"],
    ):
        for path, path_ranges in parse_changed_ranges(git_text(args, cwd=cwd)).items():
            if is_candidate(path) and (cwd / path).is_file():
                ranges.setdefault(path, []).extend(path_ranges)

    if include_untracked:
        for path in git_lines(["ls-files", "--others", "--exclude-standard"], cwd=cwd):
            if is_candidate(path) and (cwd / path).is_file():
                ranges.setdefault(path, []).append((1, line_count(cwd / path)))

    return {path: merge_ranges(path_ranges) for path, path_ranges in ranges.items()}


def all_tracked_files(cwd: Path) -> list[str]:
    return candidate_files(cwd, git_lines(["ls-files"], cwd=cwd))


def candidate_files(cwd: Path, files: Iterable[str]) -> list[str]:
    return sorted(path for path in files if is_candidate(path) and (cwd / path).is_file())


def tidy_files(files: Iterable[str]) -> list[str]:
    return sorted(path for path in files if Path(path).suffix in TIDY_SOURCE_EXTENSIONS)


def check_clang_format(
    files: Iterable[str],
    *,
    cwd: Path,
    clang_format: str,
    fix: bool,
    report: TextIO | None,
) -> int:
    file_list = list(files)
    if not file_list:
        print("clang-format: no C/C++ files to check")
        return 0

    options = ["-i"] if fix else ["--dry-run", "--Werror"]
    result = run_logged([clang_format, *options, *file_list], cwd=cwd)
    if not fix and result.returncode != 0:
        for path in file_list:
            single = run([clang_format, "--dry-run", "--Werror", path], cwd=cwd, check=False)
            if single.returncode != 0:
                print_format_diff(path, cwd=cwd, clang_format=clang_format, line_args=[], report=report)
    return result.returncode


def check_clang_format_ranges(
    file_ranges: dict[str, list[tuple[int, int]]],
    *,
    cwd: Path,
    clang_format: str,
    fix: bool,
    report: TextIO | None,
) -> int:
    if not file_ranges:
        print("clang-format: no changed C/C++ lines to check")
        return 0

    rc = 0
    for path, ranges in sorted(file_ranges.items()):
        line_args = [f"-lines={start}:{end}" for start, end in ranges]
        options = ["-i"] if fix else ["--dry-run", "--Werror"]
        result = run_logged([clang_format, *options, *line_args, path], cwd=cwd)
        if not fix and result.returncode != 0:
            print_format_diff(path, cwd=cwd, clang_format=clang_format, line_args=line_args, report=report)
        rc = result.returncode or rc
    return rc


def check_clang_tidy_config(cwd: Path, clang_tidy: str) -> int:
    result = run_logged([clang_tidy, "--config-file=.clang-tidy", "--verify-config"], cwd=cwd)
    return result.returncode


def build_dir_candidates(build_dir: str | None, repo: Path) -> list[Path]:
    candidates: list[Path] = []
    if build_dir:
        candidates.append((repo / build_dir).resolve())
    env_build_dir = os.environ.get("CINDERX_CLEAN_CODE_BUILD_DIR")
    if env_build_dir:
        candidates.append((repo / env_build_dir).resolve())
    candidates.extend(
        [
            repo / "build" / "clean-code",
            repo / "build",
            repo / "cmake-build-debug",
            repo / "cmake-build-release",
        ]
    )
    return candidates


def find_compile_commands(build_dir: str | None, repo: Path) -> Path | None:
    candidates = build_dir_candidates(build_dir, repo)

    seen: set[Path] = set()
    for candidate in candidates:
        if candidate in seen:
            continue
        seen.add(candidate)
        if (candidate / "compile_commands.json").is_file():
            return candidate
    return None


def default_build_dir(repo: Path, build_dir: str | None) -> Path:
    return build_dir_candidates(build_dir, repo)[0]


def load_cmake_feature_options(repo: Path):
    repo_text = str(repo)
    if repo_text not in sys.path:
        sys.path.insert(0, repo_text)
    from ci_pipeline.cmake_options import cmake_feature_options

    return cmake_feature_options


def runtime_tests_cmake_options(repo: Path) -> list[str]:
    python_root = os.environ.get("Python_ROOT_DIR") or sys.base_prefix
    local_deps = os.environ.get("CINDERX_LOCAL_DEPS_DIR") or os.environ.get("CINDERX_LOCAL_DEPS")
    if not local_deps:
        sibling_local_deps = repo.parent / "cinderx-local-deps"
        if sibling_local_deps.is_dir():
            local_deps = str(sibling_local_deps)

    feature_options = load_cmake_feature_options(repo)(python_root=python_root)
    options = [f"-D{name}={value}" for name, value in feature_options.items()]
    if local_deps:
        options.append(f"-DCINDERX_LOCAL_DEPS_DIR={local_deps}")

    cc = os.environ.get("CC")
    cxx = os.environ.get("CXX")
    if not cc:
        cc = default_llvm_tool("clang", "CC")
    if not cxx:
        cxx = default_llvm_tool("clang++", "CXX")
    if cc:
        options.append(f"-DCMAKE_C_COMPILER={cc}")
    if cxx:
        options.append(f"-DCMAKE_CXX_COMPILER={cxx}")
    return options


def generate_compile_commands(*, repo: Path, build_dir: Path, cmake: str) -> int:
    cmd = [
        cmake,
        "-S",
        str(repo),
        "-B",
        str(build_dir),
        "-DENABLE_RUNTIME_TESTS=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        *runtime_tests_cmake_options(repo),
    ]
    print("compile_commands.json not found; generating with CMake.")
    result = run_logged(cmd, cwd=repo)
    if result.returncode != 0:
        return result.returncode
    if not (build_dir / "compile_commands.json").is_file():
        print(
            f"error: CMake completed but {build_dir / 'compile_commands.json'} was not created",
            file=sys.stderr,
        )
        return 2
    return 0


def check_clang_tidy(
    files: Iterable[str],
    *,
    cwd: Path,
    clang_tidy: str,
    build_dir: Path,
    extra_args: list[str],
) -> int:
    file_list = tidy_files(files)
    if not file_list:
        print("clang-tidy: no changed C/C++ translation units to check")
        return 0

    rc = 0
    for path in file_list:
        cmd = [
            clang_tidy,
            "--config-file=.clang-tidy",
            "-p",
            str(build_dir),
            *extra_args,
            path,
        ]
        result = run_logged(cmd, cwd=cwd)
        rc = result.returncode or rc
    return rc


def main() -> int:
    parser = argparse.ArgumentParser(description="Run clean-code checks on C/C++ files.")
    parser.add_argument("--base", default="origin/master", help="base ref for incremental checks, default: origin/master")
    parser.add_argument("--format", action="store_true", help="run clang-format")
    parser.add_argument("--tidy", action="store_true", help="run clang-tidy")
    parser.add_argument("--fix", action="store_true", help="apply clang-format to changed files instead of dry-run checking")
    parser.add_argument(
        "--all",
        action="store_true",
        help="check all tracked C/C++ files instead of only files changed from --base",
    )
    parser.add_argument(
        "--build-dir",
        help=(
            "build directory containing compile_commands.json for clang-tidy; "
            "defaults to CINDERX_CLEAN_CODE_BUILD_DIR, build/clean-code, build, "
            "cmake-build-debug, or cmake-build-release"
        ),
    )
    parser.add_argument(
        "--no-untracked",
        action="store_true",
        help="do not include untracked files in the incremental changed-file set",
    )
    parser.add_argument(
        "--clang-format",
        default=default_llvm_tool("clang-format", "CLANG_FORMAT"),
        help=f"clang-format command, default prefers LLVM/Clang {LLVM_MAJOR_VERSION}",
    )
    parser.add_argument(
        "--clang-tidy",
        default=default_llvm_tool("clang-tidy", "CLANG_TIDY"),
        help=f"clang-tidy command, default prefers LLVM/Clang {LLVM_MAJOR_VERSION}",
    )
    parser.add_argument(
        "--clang-tidy-extra-arg",
        action="append",
        default=[],
        help="extra argument passed to clang-tidy, repeatable; values are passed as --extra-arg=<value>",
    )
    parser.add_argument("--cmake", default=os.environ.get("CMAKE", "cmake"))
    parser.add_argument(
        "--no-generate-compile-commands",
        action="store_true",
        help="fail instead of running CMake when compile_commands.json is missing",
    )
    parser.add_argument(
        "--skip-tidy-config",
        action="store_true",
        help="skip clang-tidy configuration validation",
    )
    parser.add_argument(
        "--format-report",
        help=(
            "write suggested clang-format diffs to this file; default: "
            "build/clean-code/<timestamp>/logs/clang-format.diff"
        ),
    )
    args = parser.parse_args()

    cwd = Path.cwd()
    try:
        repo = Path(run(["git", "rev-parse", "--show-toplevel"], cwd=cwd).stdout.strip())
    except subprocess.CalledProcessError as exc:
        print(exc.stderr, file=sys.stderr, end="")
        return exc.returncode

    if not args.format and not args.tidy:
        args.format = True
        args.tidy = True

    if args.format:
        rc = check_llvm_tool_version(args.clang_format, display_name="CLANG_FORMAT")
        if rc:
            return rc
    if args.tidy:
        rc = check_llvm_tool_version(args.clang_tidy, display_name="CLANG_TIDY")
        if rc:
            return rc

    format_ranges: dict[str, list[tuple[int, int]]] | None = None
    if args.all:
        files = all_tracked_files(repo)
        print("Check scope: all tracked C/C++ files")
    else:
        files = changed_files(repo, args.base, include_untracked=not args.no_untracked)
        if args.format:
            format_ranges = changed_file_ranges(repo, args.base, include_untracked=not args.no_untracked)
        print(f"Base ref: {args.base}")
        print("Check scope: changed C/C++ files")
    print(f"C/C++ files checked: {len(files)}")
    for path in files:
        print(f"  {path}")

    format_report: TextIO | None = None
    report_path: Path | None = None
    if args.format:
        report_path = (repo / args.format_report).resolve() if args.format_report else default_format_report_path(repo)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        format_report = report_path.open("w", encoding="utf-8")
        print(f"clang-format diff report: {report_path}")

    rc = 0
    try:
        if args.format:
            if args.all:
                rc = check_clang_format(
                    files,
                    cwd=repo,
                    clang_format=args.clang_format,
                    fix=args.fix,
                    report=format_report,
                ) or rc
            else:
                rc = check_clang_format_ranges(
                    format_ranges or {},
                    cwd=repo,
                    clang_format=args.clang_format,
                    fix=args.fix,
                    report=format_report,
                ) or rc

        if args.tidy and not args.skip_tidy_config:
            if shutil.which(args.clang_tidy) is None:
                print(f"warning: {args.clang_tidy!r} not found; skip clang-tidy config validation", file=sys.stderr)
            else:
                rc = check_clang_tidy_config(repo, args.clang_tidy) or rc

        if args.tidy:
            tidy_input_files = tidy_files(files)
            if tidy_input_files:
                build_dir = find_compile_commands(args.build_dir, repo)
                if build_dir is None:
                    build_dir = default_build_dir(repo, args.build_dir)
                    if args.no_generate_compile_commands:
                        print(
                            "error: compile_commands.json not found for clang-tidy. "
                            "Generate it first, for example:\n"
                            "  cmake -S . -B build/clean-code -DENABLE_RUNTIME_TESTS=ON "
                            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                            file=sys.stderr,
                        )
                        return 2
                    if shutil.which(args.cmake) is None:
                        print(f"error: {args.cmake!r} not found; install CMake or set CMAKE", file=sys.stderr)
                        return 2
                    rc = generate_compile_commands(repo=repo, build_dir=build_dir, cmake=args.cmake) or rc
                    if rc:
                        return rc
            else:
                build_dir = repo
            extra_args = ["--extra-arg=-D_Py_USE_GCC_BUILTIN_ATOMICS=1"]
            extra_args.extend(f"--extra-arg={value}" for value in args.clang_tidy_extra_arg)
            extra_args.extend(shlex.split(os.environ.get("CLANG_TIDY_EXTRA_ARGS", "")))
            rc = check_clang_tidy(
                tidy_input_files,
                cwd=repo,
                clang_tidy=args.clang_tidy,
                build_dir=build_dir,
                extra_args=extra_args,
            ) or rc
    finally:
        if format_report is not None:
            format_report.close()
            if report_path is not None:
                print(f"clang-format diff report: {report_path}")
                if rc and report_path.is_file() and report_path.stat().st_size > 0:
                    print(report_path.read_text(encoding="utf-8"), end="")

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
