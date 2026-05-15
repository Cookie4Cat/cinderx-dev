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
AUTO_IMPORT_ENABLE_ENV = "CINDERX_PLUGIN_ENABLE"
COUNT_KEYS = ("passed", "failed", "error", "skipped", "deselected")
COUNT_KEY_ALIASES = {
    "errors": "error",
    "failures": "failed",
    "failure": "failed",
}
COVERAGE_TOOLS = ("gcov", "lcov", "genhtml")

# Minimum coverage percentages enforced by --coverage after final lcov filters.
# Tune these values when the accepted project baseline changes.
COVERAGE_MIN_PERCENT = {
    "line": 50.0,
    "function": 60.0,
    "branch": 30.0,
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


def merged_env(job: dict[str, Any], coverage: bool = False) -> dict[str, str]:
    env = os.environ.copy()
    configure_toolchain(env)
    if coverage:
        env["CINDERX_ENABLE_COVERAGE"] = "1"
        env[AUTO_IMPORT_ENABLE_ENV] = "1"
    for key, value in job.get("env", {}).items():
        env[str(key)] = str(value)
    return env


def coverage_tool_paths() -> dict[str, str]:
    paths: dict[str, str] = {}
    missing = []
    for tool in COVERAGE_TOOLS:
        path = shutil.which(tool)
        if path:
            paths[tool] = path
        else:
            missing.append(tool)
    if missing:
        raise RuntimeError(
            "coverage mode requires missing tool(s): " + ", ".join(missing)
        )
    return paths


def lcov_version(tool_path: str) -> tuple[int, int] | None:
    try:
        completed = subprocess.run(
            [tool_path, "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except OSError:
        return None
    match = re.search(r"LCOV version\s+([0-9]+)(?:\.([0-9]+))?", completed.stdout)
    if not match:
        return None
    major = int(match.group(1))
    minor = int(match.group(2) or 0)
    return major, minor


def lcov_is_v2_or_newer(tool_path: str) -> bool:
    version = lcov_version(tool_path)
    return bool(version and version[0] >= 2)


def lcov_branch_coverage_args(lcov_v2_or_newer: bool) -> list[str]:
    if lcov_v2_or_newer:
        return ["--rc", "branch_coverage=1"]
    return ["--rc", "lcov_branch_coverage=1"]


def lcov_ignore_errors_args(
    lcov_v2_or_newer: bool,
    errors: tuple[str, ...],
) -> list[str]:
    if not lcov_v2_or_newer or not errors:
        return []
    return ["--ignore-errors", ",".join(errors)]


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


def coverage_clean_native_build_command() -> str:
    return (
        "if [ -d build ]; then "
        "find build -mindepth 1 -maxdepth 1 "
        "! -name testgate ! -name fbcode_builder "
        "-exec rm -rf {} +; "
        "fi && rm -rf scratch cinderx.egg-info"
    )


def job_uses_coverage(job: dict[str, Any], suite_coverage: bool) -> bool:
    if not suite_coverage:
        return False
    default = str(job.get("kind", "command")) == "runtime_tests"
    return bool(job.get("coverage", default))


def clean_stale_runtime_test_builds(run_dir: Path) -> None:
    if not ARTIFACT_ROOT.exists():
        return
    for entry in ARTIFACT_ROOT.iterdir():
        stale_runtime_build = entry / "runtime-tests-build"
        if entry.is_dir() and entry != run_dir and stale_runtime_build.exists():
            shutil.rmtree(stale_runtime_build)


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
    if env.get("CINDERX_ENABLE_COVERAGE") == "1":
        cmake_args.append("-DENABLE_COVERAGE=ON")
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
    coverage: bool = False,
) -> str:
    kind = str(job.get("kind", "command"))
    if kind == "command":
        command = str(job["command"]).format(
            repo=REPO_ROOT,
            run_dir=run_dir,
        )
        if coverage and str(job.get("name")) == "setup_release":
            command = f"{coverage_clean_native_build_command()} && {command}"
    elif kind == "runtime_tests":
        command = runtime_tests_command(job, run_dir, env)
        if coverage:
            command = f"{coverage_clean_native_build_command()} && {command}"
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


def run_job(
    job: dict[str, Any],
    run_dir: Path,
    prelude: str,
    coverage: bool = False,
) -> dict[str, Any]:
    name = str(job["name"])
    log_path = run_dir / "logs" / f"{name}.log"

    started = _datetime.datetime.now().isoformat(timespec="seconds")
    print(f"[ RUN      ] {name}", flush=True)
    env = merged_env(job, coverage)
    try:
        command = command_for_job(job, run_dir, prelude, env, coverage)
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


def relative_to_repo(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def parse_lcov_summary_metrics(text: str) -> dict[str, str]:
    metrics = {}
    labels = {
        "lines": "line",
        "functions": "function",
        "branches": "branch",
    }
    for genhtml_label, summary_label in labels.items():
        match = re.search(rf"{genhtml_label}\.+:\s*([^\n]+)", text)
        if match:
            metrics[summary_label] = match.group(1).strip()
    return metrics


def format_coverage_metric(hit: int, total: int, units: str) -> str:
    percent = 100.0 if total == 0 else hit * 100.0 / total
    return f"{percent:.1f}% ({hit} of {total} {units})"


def parse_lcov_tracefile_metrics(info_path: Path) -> dict[str, str]:
    totals = {
        "line": 0,
        "function": 0,
        "branch": 0,
    }
    hits = {
        "line": 0,
        "function": 0,
        "branch": 0,
    }
    fields = {
        "LF": ("line", totals),
        "LH": ("line", hits),
        "FNF": ("function", totals),
        "FNH": ("function", hits),
        "BRF": ("branch", totals),
        "BRH": ("branch", hits),
    }
    seen_totals: set[str] = set()

    try:
        with info_path.open(encoding="utf-8", errors="replace") as info_file:
            for line in info_file:
                key, separator, value = line.partition(":")
                if not separator:
                    continue
                field = fields.get(key)
                if field is None:
                    continue
                metric, bucket = field
                try:
                    bucket[metric] += int(value.strip())
                    if bucket is totals:
                        seen_totals.add(metric)
                except ValueError:
                    continue
    except OSError:
        return {}

    if not seen_totals:
        return {}

    metrics = {}
    if "line" in seen_totals:
        metrics["line"] = format_coverage_metric(
            hits["line"],
            totals["line"],
            "lines",
        )
    if "function" in seen_totals:
        metrics["function"] = format_coverage_metric(
            hits["function"],
            totals["function"],
            "functions",
        )
    if "branch" in seen_totals:
        metrics["branch"] = format_coverage_metric(
            hits["branch"],
            totals["branch"],
            "branches",
        )
    return metrics


def parse_coverage_percent(value: str | None) -> float | None:
    if not value:
        return None
    match = re.match(r"\s*([0-9]+(?:\.[0-9]+)?)%", value)
    if not match:
        return None
    return float(match.group(1))


def coverage_threshold_failures(
    metrics: dict[str, str],
) -> list[dict[str, Any]]:
    failures = []
    for metric, minimum in COVERAGE_MIN_PERCENT.items():
        value = metrics.get(metric)
        actual = parse_coverage_percent(value)
        if actual is None:
            failures.append(
                {
                    "metric": metric,
                    "minimum": minimum,
                    "actual": None,
                    "value": value or "missing",
                }
            )
        elif actual < minimum:
            failures.append(
                {
                    "metric": metric,
                    "minimum": minimum,
                    "actual": actual,
                    "value": value,
                }
            )
    return failures


def format_coverage_threshold_failure(failure: dict[str, Any]) -> str:
    metric = failure["metric"]
    minimum = failure["minimum"]
    actual = failure["actual"]
    value = failure["value"]
    if actual is None:
        return f"{metric}: missing coverage metric, minimum {minimum:.1f}%"
    return f"{metric}: {value} is below minimum {minimum:.1f}%"


def write_coverage_threshold_report(
    metrics: dict[str, str],
    failures: list[dict[str, Any]],
    log_path: Path,
) -> None:
    failed_metrics = {failure["metric"] for failure in failures}
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write("\n[thresholds]\n")
        for metric, minimum in COVERAGE_MIN_PERCENT.items():
            value = metrics.get(metric)
            actual = parse_coverage_percent(value)
            if actual is None:
                log_file.write(
                    f"{metric}: missing, minimum {minimum:.1f}% [FAILED]\n"
                )
                continue
            status = "FAILED" if metric in failed_metrics else "OK"
            log_file.write(
                f"{metric}: {value}, minimum {minimum:.1f}% [{status}]\n"
            )


def run_logged_step(step: str, args: list[str], cwd: Path, log_path: Path) -> int:
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write(f"\n[{step}]\n")
        log_file.write("$ " + shell_join(args) + "\n\n")
        log_file.flush()
        completed = subprocess.run(
            args,
            cwd=cwd,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        log_file.write(f"\nexit code: {completed.returncode}\n")
    return completed.returncode


def collect_lcov_summary_metrics(
    coverage_info: Path,
    log_path: Path,
) -> dict[str, str]:
    metrics = parse_lcov_tracefile_metrics(coverage_info)
    with log_path.open("a", encoding="utf-8") as log_file:
        log_file.write("\n[summary]\n")
        log_file.write(f"source: {coverage_info}\n\n")
        if metrics:
            log_file.write("Summary coverage rate:\n")
            for label, metric in (
                ("lines", "line"),
                ("functions", "function"),
                ("branches", "branch"),
            ):
                value = metrics.get(metric, "missing")
                log_file.write(f"  {label.ljust(12, '.')}: {value}\n")
        else:
            log_file.write("Unable to parse coverage metrics from tracefile.\n")
    return metrics


def generate_coverage_report(
    run_dir: Path,
    tool_paths: dict[str, str],
) -> dict[str, Any]:
    coverage_dir = run_dir / "coverage"
    html_dir = coverage_dir / "html"
    raw_info = coverage_dir / "coverage.raw.info"
    coverage_info = coverage_dir / "coverage.info"
    log_path = run_dir / "logs" / "coverage.log"

    coverage_dir.mkdir(parents=True, exist_ok=True)
    html_dir.mkdir(parents=True, exist_ok=True)
    log_path.write_text("", encoding="utf-8")

    result: dict[str, Any] = {
        "enabled": True,
        "status": "failed",
        "info": relative_to_repo(coverage_info),
        "html_index": relative_to_repo(html_dir / "index.html"),
        "log": relative_to_repo(log_path),
        "tools": tool_paths,
        "thresholds": COVERAGE_MIN_PERCENT,
    }

    lcov_v2_or_newer = lcov_is_v2_or_newer(tool_paths["lcov"])
    common_lcov_args = lcov_branch_coverage_args(lcov_v2_or_newer)
    capture_args = [
        tool_paths["lcov"],
        "--capture",
        "--directory",
        str(REPO_ROOT),
        "--output-file",
        str(raw_info),
        "--gcov-tool",
        tool_paths["gcov"],
        "--no-external",
        *lcov_ignore_errors_args(
            lcov_v2_or_newer,
            ("mismatch", "inconsistent"),
        ),
        *common_lcov_args,
    ]
    remove_args = [
        tool_paths["lcov"],
        "--remove",
        str(raw_info),
        "*/cinderx/ThirdParty/*",
        "*/cinderx/RuntimeTests/*",
        "*/cinderx/TestScripts/*",
        "*/cinderx/PythonLib/test_cinderx/*",
        "*/_deps/*",
        "*/scratch/*",
        "*/build/*",
        *lcov_ignore_errors_args(
            lcov_v2_or_newer,
            ("inconsistent", "unused"),
        ),
        "--output-file",
        str(coverage_info),
        *common_lcov_args,
    ]
    genhtml_args = [
        tool_paths["genhtml"],
        str(coverage_info),
        "--output-directory",
        str(html_dir),
        "--branch-coverage",
        *lcov_ignore_errors_args(
            lcov_v2_or_newer,
            ("inconsistent", "corrupt"),
        ),
        "--title",
        f"CinderX coverage {run_dir.name}",
    ]
    steps = [
        ("capture", capture_args),
        ("filter", remove_args),
        ("html", genhtml_args),
    ]
    for step, args in steps:
        print(f"[ COVERAGE ] {step}", flush=True)
        returncode = run_logged_step(step, args, REPO_ROOT, log_path)
        if returncode != 0:
            result.update(
                {
                    "failed_step": step,
                    "returncode": returncode,
                }
            )
            print(f"[  FAILED ] coverage {step} ({log_path})", flush=True)
            return result

    metrics = collect_lcov_summary_metrics(
        coverage_info,
        log_path,
    )
    result["metrics"] = metrics
    threshold_failures = coverage_threshold_failures(metrics)
    print("[ COVERAGE ] thresholds", flush=True)
    write_coverage_threshold_report(metrics, threshold_failures, log_path)
    if threshold_failures:
        result.update(
            {
                "failed_step": "thresholds",
                "threshold_failures": threshold_failures,
            }
        )
        print(f"[  FAILED ] coverage thresholds ({log_path})", flush=True)
        for failure in threshold_failures:
            print(
                f"  - {format_coverage_threshold_failure(failure)}",
                flush=True,
            )
        return result

    result["status"] = "passed"
    print("[       OK ] coverage thresholds", flush=True)
    print(f"[       OK ] coverage html ({html_dir / 'index.html'})", flush=True)
    return result


def write_summary(
    run_dir: Path,
    suite_name: str,
    results: list[dict[str, Any]],
    coverage: dict[str, Any] | None = None,
) -> Path:
    failed = [result for result in results if result["returncode"] != 0]
    coverage_failed = bool(
        coverage
        and coverage.get("enabled")
        and coverage.get("status") == "failed"
    )
    summary = {
        "suite": suite_name,
        "status": "failed" if failed or coverage_failed else "passed",
        "repo": str(REPO_ROOT),
        "head": git_head(),
        "results": results,
        "coverage": coverage or {"enabled": False},
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
    parser.add_argument(
        "--coverage",
        action="store_true",
        help=(
            "build native C/C++ code with GCC coverage instrumentation and "
            "write lcov/genhtml reports under the run artifact directory"
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

    coverage_tools: dict[str, str] | None = None
    if args.coverage:
        try:
            coverage_tools = coverage_tool_paths()
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2

    run_dir = make_run_dir(args.suite)
    print(f"artifact directory: {run_dir}", flush=True)
    if args.coverage:
        clean_stale_runtime_test_builds(run_dir)
        print("coverage: enabled (GCC/gcov + lcov/genhtml)", flush=True)

    results = []
    prelude = str(suite.get("prelude", ""))
    if "CINDERX_TESTGATE_PRELUDE" in os.environ:
        prelude = os.environ["CINDERX_TESTGATE_PRELUDE"]
    if args.prelude is not None:
        prelude = args.prelude
    fail_fast = bool(suite.get("fail_fast", True))
    for job in jobs:
        result = run_job(job, run_dir, prelude, job_uses_coverage(job, args.coverage))
        results.append(result)
        if fail_fast and result["returncode"] != 0:
            break

    coverage_result: dict[str, Any] = {"enabled": False}
    if args.coverage:
        assert coverage_tools is not None
        coverage_result = generate_coverage_report(run_dir, coverage_tools)

    summary_path = write_summary(run_dir, args.suite, results, coverage_result)
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
    if coverage_result.get("enabled"):
        print("Coverage:", flush=True)
        print("", flush=True)
        metrics = coverage_result.get("metrics") or {}
        metric_parts = [
            f"{label} {metrics[key]}"
            for key, label in (
                ("line", "line"),
                ("function", "function"),
                ("branch", "branch"),
            )
            if key in metrics
        ]
        if metric_parts:
            print(f"    metrics: {', '.join(metric_parts)}", flush=True)
        html_index = coverage_result.get("html_index")
        if html_index:
            print(f"    html: {html_index}", flush=True)
        log = coverage_result.get("log")
        if log:
            print(f"    log: {log}", flush=True)
        print("", flush=True)
    print(f"{'=' * 60}", flush=True)

    if failed:
        print("failed jobs:", flush=True)
        for result in failed:
            print(f"  - {result['name']} ({result['log']})", flush=True)
        return 1

    if coverage_result.get("status") == "failed":
        print(
            f"coverage failed: {coverage_result.get('log', 'see coverage log')}",
            flush=True,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
