#!/usr/bin/env python3
"""Run local CinderX merge gates."""

from __future__ import annotations

import argparse
import datetime as _datetime
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tomllib
from typing import Any


def find_repo_root() -> Path:
    path = Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "cinderx").is_dir():
            return parent
    raise RuntimeError("could not find repository root")


REPO_ROOT = find_repo_root()
TESTGATE_DIR = REPO_ROOT / "ci_pipeline"
SUITES_DIR = TESTGATE_DIR / "suites"
ARTIFACT_ROOT = REPO_ROOT / "build" / "testgate"
ALLOW_TARGET_MISMATCH_ENV = "CINDERX_TESTGATE_ALLOW_TARGET_MISMATCH"
COUNT_KEYS = ("passed", "failed", "error", "skipped", "deselected")
COUNT_KEY_ALIASES = {
    "errors": "error",
    "failures": "failed",
    "failure": "failed",
}


def load_suite(name: str) -> dict[str, Any]:
    suite_path = SUITES_DIR / f"{name}.toml"
    if not suite_path.exists():
        raise FileNotFoundError(f"suite not found: {suite_path}")
    with suite_path.open("rb") as suite_file:
        data = tomllib.load(suite_file)
    jobs = data.get("jobs")
    if not isinstance(jobs, list) or not jobs:
        raise ValueError(f"suite {suite_path} must define at least one [[jobs]] entry")
    return data


def check_target(expected: dict[str, Any]) -> list[str]:
    warnings: list[str] = []

    expected_python = str(expected.get("python", ""))
    if expected_python:
        actual_python = f"{sys.version_info.major}.{sys.version_info.minor}"
        if actual_python != expected_python:
            warnings.append(
                f"Python version is {actual_python}, expected {expected_python}"
            )

    expected_arch = str(expected.get("arch", "")).lower()
    if expected_arch:
        actual_arch = platform.machine().lower()
        aliases = {
            "arm64": {"arm64", "aarch64"},
            "aarch64": {"arm64", "aarch64"},
        }
        accepted = aliases.get(expected_arch, {expected_arch})
        if actual_arch not in accepted:
            warnings.append(f"machine is {actual_arch}, expected {expected_arch}")

    expected_system = str(expected.get("system", "")).lower()
    if expected_system:
        actual_system = platform.system().lower()
        if actual_system != expected_system:
            warnings.append(f"system is {actual_system}, expected {expected_system}")

    return warnings


def allow_target_mismatch(args: argparse.Namespace) -> bool:
    return bool(args.allow_target_mismatch) or os.environ.get(
        ALLOW_TARGET_MISMATCH_ENV
    ) == "1"


def timestamp() -> str:
    return _datetime.datetime.now().strftime("%Y%m%d-%H%M%S")


def make_run_dir(suite_name: str) -> Path:
    run_dir = ARTIFACT_ROOT / f"{suite_name}-{timestamp()}"
    (run_dir / "logs").mkdir(parents=True, exist_ok=False)
    return run_dir


def first_executable(candidates: list[str], extra_globs: list[str]) -> str | None:
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return path
    for pattern in extra_globs:
        for path in sorted(Path("/").glob(pattern), reverse=True):
            if path.is_file() and os.access(path, os.X_OK):
                return str(path)
    return None


def configure_toolchain(env: dict[str, str]) -> None:
    env.setdefault("CINDERX_TEST_PYTHON", sys.executable)

    if "CC" not in env:
        cc = first_executable(
            ["gcc-14", "gcc", "clang-19", "clang"],
            [
                "opt/gcc-*/bin/gcc",
                "opt/clang-*/bin/clang",
            ],
        )
        if cc:
            env["CC"] = cc

    if "CXX" not in env:
        cxx = first_executable(
            ["g++-14", "g++", "clang++-19", "clang++"],
            [
                "opt/gcc-*/bin/g++",
                "opt/clang-*/bin/clang++",
            ],
        )
        if cxx:
            env["CXX"] = cxx


def merged_env(job: dict[str, Any]) -> dict[str, str]:
    env = os.environ.copy()
    configure_toolchain(env)
    for key, value in job.get("env", {}).items():
        env[str(key)] = str(value)
    return env


def cmake_value(value: object) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        return value
    raise ValueError(f"unsupported CMake option value: {value!r}")


def cinderx_test_python_info(env: dict[str, str]) -> dict[str, Any]:
    code = (
        "import json, os, sys, sysconfig; "
        "print(json.dumps({"
        "'py_version': f'{sys.version_info.major}.{sys.version_info.minor}', "
        "'python_root': os.path.join(sysconfig.get_path('include'), '..', '..'), "
        "'meta_python': '+meta' in sys.version, "
        "'linux': sys.platform == 'linux', "
        "'mac': sys.platform == 'darwin'"
        "}))"
    )
    completed = subprocess.run(
        [env["CINDERX_TEST_PYTHON"], "-c", code],
        cwd=REPO_ROOT,
        env=env,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return json.loads(completed.stdout)


def runtime_tests_cmake_options(env: dict[str, str]) -> list[str]:
    info = cinderx_test_python_info(env)
    py_version = str(info["py_version"])
    meta_python = bool(info["meta_python"])
    linux = bool(info["linux"])
    mac = bool(info["mac"])
    meta_312 = meta_python and py_version == "3.12"
    is_314plus = py_version in {"3.14", "3.15"}

    options: dict[str, str] = {
        "PY_VERSION": py_version,
        "Python_ROOT_DIR": str(info["python_root"]),
    }

    def set_option(var: str, default: object) -> None:
        options[var] = env.get(var, cmake_value(default))

    set_option("META_PYTHON", meta_python)
    set_option("ENABLE_ADAPTIVE_STATIC_PYTHON", meta_312)
    set_option("ENABLE_DISASSEMBLER", True)
    set_option("ENABLE_ELF_READER", linux)
    set_option("ENABLE_EVAL_HOOK", meta_312)
    set_option("ENABLE_FUNC_EVENT_MODIFY_QUALNAME", meta_312)
    set_option("ENABLE_GENERATOR_AWAITER", meta_312)
    set_option("ENABLE_INTERPRETER_LOOP", meta_312 or is_314plus)
    set_option("ENABLE_LAZY_IMPORTS", meta_312)
    set_option("ENABLE_LIGHTWEIGHT_FRAMES", meta_312)
    set_option("ENABLE_PARALLEL_GC", meta_312)
    set_option("ENABLE_PEP523_HOOK", meta_312 or is_314plus)
    set_option("ENABLE_PERF_TRAMPOLINE", meta_312)
    set_option("ENABLE_SYMBOLIZER", linux)
    set_option("ENABLE_USDT", linux)
    set_option("ENABLE_XXCLASSLOADER", False)
    set_option("ENABLE_ZLIB", linux or mac)

    return [f"-D{name}={value}" for name, value in options.items()]


def shell_join(args: list[str]) -> str:
    return " ".join(shlex.quote(str(arg)) for arg in args)


def runtime_tests_command(
    job: dict[str, Any],
    run_dir: Path,
    env: dict[str, str],
) -> str:
    build_dir = run_dir / "runtime-tests-build"
    build_type = env.get("CMAKE_BUILD_TYPE", "RelWithDebInfo")
    verbose_makefile = env.get("CMAKE_VERBOSE_MAKEFILE", "OFF")
    parallelism = env.get("CINDERX_TEST_JOBS", str(os.cpu_count() or 2))

    cmake_args = [
        "cmake",
        "-S",
        str(REPO_ROOT),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_VERBOSE_MAKEFILE:BOOL={verbose_makefile}",
        "-DENABLE_RUNTIME_TESTS=ON",
        (
            "-DENABLE_LTO=ON"
            if env.get("CINDERX_ENABLE_LTO") is not None
            else "-DENABLE_LTO=OFF"
        ),
        *runtime_tests_cmake_options(env),
    ]
    if env.get("CC"):
        cmake_args.append(f"-DCMAKE_C_COMPILER={env['CC']}")
    if env.get("CXX"):
        cmake_args.append(f"-DCMAKE_CXX_COMPILER={env['CXX']}")

    build_args = [
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        str(job.get("target", "runtime_tests")),
        "--config",
        build_type,
        "--parallel",
        parallelism,
    ]
    ctest_command = (
        f"cd {shlex.quote(str(build_dir))} && "
        f"{shell_join(['ctest', '--output-on-failure', '-C', build_type])}"
    )

    return " && ".join(
        [
            shell_join(["rm", "-rf", str(build_dir)]),
            shell_join(cmake_args),
            shell_join(build_args),
            ctest_command,
        ]
    )


def command_for_job(
    job: dict[str, Any],
    run_dir: Path,
    prelude: str,
    env: dict[str, str],
) -> str:
    kind = str(job.get("kind", "command"))
    if kind == "command":
        command = str(job["command"]).format(
            repo=REPO_ROOT,
            run_dir=run_dir,
        )
    elif kind == "runtime_tests":
        command = runtime_tests_command(job, run_dir, env)
    else:
        raise ValueError(
            f"unknown job kind for {job.get('name', '<unnamed>')}: {kind}"
        )

    if not prelude:
        return command
    return f"set -euo pipefail; {prelude}; {command}"


def normalize_count_key(key: str) -> str | None:
    normalized = COUNT_KEY_ALIASES.get(key, key)
    if normalized in COUNT_KEYS:
        return normalized
    return None


def parse_pytest_summary(log_path: Path) -> dict[str, int] | None:
    """Parse test counts from a log file.

    Supports:
    1. Standard pytest:  ======= 42 passed, 3 failed in 12.34s =======
    2. test_cinderx_runner aggregated:  Tests:  203 collected, 197 passed, ...
    3. Individual suite lines:  [       OK ] name [42 passed, 3 skipped] (path)
    """
    counts = {key: 0 for key in COUNT_KEYS}
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None

    found = False

    # Pattern 1: standard pytest summary line
    last_pytest = None
    for m in re.finditer(r"=+ (.+?) in [\d.]+s =+", text):
        last_pytest = m
    if last_pytest:
        found = True
        for m in re.finditer(r"(\d+) (\w+)", last_pytest.group(1)):
            key = normalize_count_key(m.group(2))
            if key:
                counts[key] = int(m.group(1))
        return counts

    # Pattern 2: test_cinderx_runner.py aggregated summary
    tests_match = re.search(r"Tests:\s+(.+)", text)
    if tests_match:
        found = True
        for m in re.finditer(r"(\d+) (\w+)", tests_match.group(1)):
            key = normalize_count_key(m.group(2))
            if key:
                counts[key] = int(m.group(1))
        return counts

    # Pattern 3: sum up individual [OK/FAILED] name [N passed, M skipped] lines
    for m in re.finditer(r"\[(?:\s+OK|\s+FAILED)\s+\]\s+\S+\s+\[([^\]]+)\]", text):
        found = True
        for pair in re.finditer(r"(\d+) (\w+)", m.group(1)):
            key = normalize_count_key(pair.group(2))
            if key:
                counts[key] += int(pair.group(1))

    return counts if found else None


def run_job(job: dict[str, Any], run_dir: Path, prelude: str) -> dict[str, Any]:
    name = str(job["name"])
    log_path = run_dir / "logs" / f"{name}.log"

    started = _datetime.datetime.now().isoformat(timespec="seconds")
    print(f"[ RUN      ] {name}", flush=True)
    env = merged_env(job)
    try:
        command = command_for_job(job, run_dir, prelude, env)
    except Exception as exc:
        finished = _datetime.datetime.now().isoformat(timespec="seconds")
        with log_path.open("w", encoding="utf-8") as log_file:
            log_file.write(f"failed to prepare job command: {exc}\n")
        print(f"[  FAILED ] {name} ({log_path})", flush=True)
        return {
            "name": name,
            "status": "failed",
            "returncode": 1,
            "command": None,
            "log": str(log_path.relative_to(REPO_ROOT)),
            "started": started,
            "finished": finished,
            "test_counts": None,
        }

    with log_path.open("w", encoding="utf-8") as log_file:
        log_file.write(f"$ {command}\n\n")
        log_file.flush()
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=env,
            executable="/bin/bash" if os.name != "nt" else None,
            shell=True,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )

    finished = _datetime.datetime.now().isoformat(timespec="seconds")
    status = "passed" if completed.returncode == 0 else "failed"
    marker = "       OK" if completed.returncode == 0 else "  FAILED"

    test_counts = parse_pytest_summary(log_path)
    if test_counts:
        parts = []
        for key in COUNT_KEYS:
            if test_counts[key]:
                parts.append(f"{test_counts[key]} {key}")
        counts_str = ", ".join(parts) if parts else "0 tests"
        print(f"[{marker} ] {name} [{counts_str}] ({log_path})", flush=True)
    else:
        print(f"[{marker} ] {name} ({log_path})", flush=True)

    return {
        "name": name,
        "status": status,
        "returncode": completed.returncode,
        "command": command,
        "log": str(log_path.relative_to(REPO_ROOT)),
        "started": started,
        "finished": finished,
        "test_counts": test_counts,
    }


def write_summary(run_dir: Path, suite_name: str, results: list[dict[str, Any]]) -> Path:
    failed = [result for result in results if result["returncode"] != 0]
    summary = {
        "suite": suite_name,
        "status": "failed" if failed else "passed",
        "repo": str(REPO_ROOT),
        "head": git_head(),
        "results": results,
    }
    summary_path = run_dir / "summary.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary_path


def git_head() -> str | None:
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", help="suite name, for example: pr")
    parser.add_argument(
        "--prelude",
        help=(
            "shell snippet to run before each job; overrides the suite prelude. "
            "Use an empty string to disable it."
        ),
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list jobs without running them",
    )
    parser.add_argument(
        "--allow-target-mismatch",
        action="store_true",
        help=(
            "continue even if the current Python/platform does not match the "
            "suite target"
        ),
    )
    args = parser.parse_args(argv)

    suite = load_suite(args.suite)
    target_warnings = check_target(suite.get("target", {}))
    for warning in target_warnings:
        print(f"warning: {warning}", file=sys.stderr)
    if target_warnings:
        if not allow_target_mismatch(args):
            print(
                "error: target environment mismatch; pass "
                "--allow-target-mismatch or set "
                f"{ALLOW_TARGET_MISMATCH_ENV}=1 to continue",
                file=sys.stderr,
            )
            return 2
        print("warning: target mismatch override enabled", file=sys.stderr)

    jobs = suite["jobs"]
    if args.list:
        for job in jobs:
            print(job["name"])
        return 0

    run_dir = make_run_dir(args.suite)
    print(f"artifact directory: {run_dir}", flush=True)

    results = []
    prelude = str(suite.get("prelude", ""))
    if "CINDERX_TESTGATE_PRELUDE" in os.environ:
        prelude = os.environ["CINDERX_TESTGATE_PRELUDE"]
    if args.prelude is not None:
        prelude = args.prelude
    fail_fast = bool(suite.get("fail_fast", True))
    for job in jobs:
        result = run_job(job, run_dir, prelude)
        results.append(result)
        if fail_fast and result["returncode"] != 0:
            break

    summary_path = write_summary(run_dir, args.suite, results)
    print(f"summary: {summary_path}", flush=True)

    # Aggregate test-level counts from all jobs that had pytest output
    totals = {key: 0 for key in COUNT_KEYS}
    has_test_counts = False
    for r in results:
        tc = r.get("test_counts")
        if tc:
            has_test_counts = True
            for key in totals:
                totals[key] += tc.get(key, 0)

    total_jobs = len(jobs)
    ran_jobs = len(results)
    passed_jobs = sum(1 for r in results if r["returncode"] == 0)
    failed = [result for result in results if result["returncode"] != 0]
    skipped_jobs = total_jobs - ran_jobs

    print(f"\n{'=' * 60}", flush=True)
    print(f"Jobs:  {ran_jobs}/{total_jobs} ran, {passed_jobs} passed, {len(failed)} failed, {skipped_jobs} skipped", flush=True)
    if has_test_counts:
        total_tests = totals["passed"] + totals["failed"] + totals["error"] + totals["skipped"]
        print(f"Tests: {total_tests} collected, {totals['passed']} passed, {totals['failed']} failed, {totals['error']} error, {totals['skipped']} skipped, {totals['deselected']} deselected", flush=True)
    print(f"{'=' * 60}", flush=True)

    if failed:
        print("failed jobs:", flush=True)
        for result in failed:
            print(f"  - {result['name']} ({result['log']})", flush=True)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
