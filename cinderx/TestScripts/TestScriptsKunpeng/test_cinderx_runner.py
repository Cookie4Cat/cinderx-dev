#!/usr/bin/env python3

from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

def find_repo_root() -> Path:
    path = Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "cinderx").is_dir():
            return parent
    raise RuntimeError("could not find repository root")

REPO_ROOT = find_repo_root()
TEST_CINDERX_DIR = REPO_ROOT / "cinderx" / "PythonLib" / "test_cinderx"
KUNPENG_TEST_CINDERX_DIR = TEST_CINDERX_DIR / "test_kunpeng"
ALL_TEST_CINDERX = [
    str(path.relative_to(REPO_ROOT)) for path in sorted(TEST_CINDERX_DIR.glob("test*.py"))
]
if KUNPENG_TEST_CINDERX_DIR.exists():
    # Keep Kunpeng-specific test discovery flat and explicit.
    ALL_TEST_CINDERX.extend(
        str(path.relative_to(REPO_ROOT))
        for path in sorted(KUNPENG_TEST_CINDERX_DIR.glob("test*.py"))
    )
COUNT_KEYS = ("passed", "failed", "error", "skipped", "deselected")
COUNT_KEY_ALIASES = {
    "errors": "error",
    "failures": "failed",
    "failure": "failed",
}

INSTRUMENTATION_FILTER = (
    "JitMonitoringIntegrationTest or JitSetProfileIntegrationTest or "
    "(JitSetTraceIntegrationTest and not "
    "test_looping_thread_deopted_on_instrumentation) or "
    "JitCombinedTracingIntegrationTest or "
    "test_suspended_generator_deopted_on_instrumentation_attach or "
    "test_multiple_suspended_generators_all_deopted"
)

SUITES = [
    {
        "name": "all_test_cinderx",
        "args": ["-m", "pytest", *ALL_TEST_CINDERX],
    },
    {
        "name": "test_cinderjit",
        "args": [
            "-m",
            "pytest",
            "-vv",
            "-rs",
            "--import-mode=importlib",
            "cinderx/PythonLib/test_cinderx/test_cinderjit.py",
        ],
        "allow_oss": True,
    },
]

for test_name in [
    "test_coro_extensions.py",
    "test_jit_coroutines.py",
    "test_jit_attr_cache.py",
    "test_parallel_gc.py",
    "test_perf_profiler_precompile.py",
    "test_type_cache.py",
]:
    SUITES.append(
        {
            "name": test_name.removesuffix(".py"),
            "args": [
                "-m",
                "pytest",
                "-vv",
                "-rs",
                "--import-mode=importlib",
                f"cinderx/PythonLib/test_cinderx/{test_name}",
            ],
            "allow_oss": True,
        }
    )

SUITES.extend(
    [
        {
            "name": "test_jit_support_instrumentation",
            "args": [
                "-m",
                "pytest",
                "-vv",
                "-rs",
                "--import-mode=importlib",
                "cinderx/PythonLib/test_cinderx/test_jit_support_instrumentation.py",
                "-k",
                INSTRUMENTATION_FILTER,
            ],
            "allow_oss": True,
            "env": {
                "PYTHONJITSUPPORTINSTRUMENTATION": "1",
                "PYTHON_JIT": "0",
            },
        },
        {
            "name": "test_frame_evaluator_clean_slate",
            "args": [
                "-m",
                "pytest",
                "-vv",
                "-rs",
                "--import-mode=importlib",
                "cinderx/PythonLib/test_cinderx/test_frame_evaluator.py",
            ],
        },
    ]
)


def normalize_count_key(key: str) -> str | None:
    normalized = COUNT_KEY_ALIASES.get(key, key)
    if normalized in COUNT_KEYS:
        return normalized
    return None


def parse_pytest_summary(log_path: Path) -> dict[str, int]:
    """Parse the pytest summary line from a log file.

    Looks for lines like:
        ======= 42 passed, 3 failed, 1 skipped, 2 deselected in 12.34s =======
        ======= 10 passed in 1.23s =======
    """
    counts = {key: 0 for key in COUNT_KEYS}
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return counts
    # Find the last pytest summary line
    for match in re.finditer(r"=+ (.+?) in [\d.]+s =+", text):
        summary = match.group(1)
    else:
        # `summary` will hold the last match if any were found
        pass
    last_match = None
    for m in re.finditer(r"=+ (.+?) in [\d.]+s =+", text):
        last_match = m
    if last_match:
        summary = last_match.group(1)
        for m in re.finditer(r"(\d+) (\w+)", summary):
            key = normalize_count_key(m.group(2))
            if key:
                counts[key] = int(m.group(1))
    return counts


def log_name(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def run_suite(suite: dict, python: str, log_dir: Path) -> dict:
    name = suite["name"]
    log_path = log_dir / f"{name}.log"
    env = os.environ.copy()
    if suite.get("allow_oss"):
        env["CINDERX_TEST_ALLOW_OSS_IMPORTS"] = "1"
    env.update(suite.get("env", {}))
    command = [python, *suite["args"]]
    print(f"[ RUN      ] {name}", flush=True)
    with log_path.open("w", encoding="utf-8") as log_file:
        log_file.write("$ " + " ".join(command) + "\n\n")
        log_file.flush()
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            env=env,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
    test_counts = parse_pytest_summary(log_path)
    parts = []
    for key in COUNT_KEYS:
        if test_counts[key]:
            parts.append(f"{test_counts[key]} {key}")
    counts_str = ", ".join(parts) if parts else "no test results parsed"

    marker = "       OK" if completed.returncode == 0 else "  FAILED"
    print(f"[{marker} ] {name} [{counts_str}] ({log_path})", flush=True)
    return {
        "name": name,
        "status": "passed" if completed.returncode == 0 else "failed",
        "returncode": completed.returncode,
        "command": command,
        "log": log_name(log_path),
        "test_counts": test_counts,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--json-summary-file", required=True)
    args = parser.parse_args(argv)

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    results = []
    for suite in SUITES:
        result = run_suite(suite, args.python, log_dir)
        results.append(result)
        if result["returncode"] != 0:
            break

    # Aggregate test-level counts across all suites
    totals = {key: 0 for key in COUNT_KEYS}
    for r in results:
        for key in totals:
            totals[key] += r.get("test_counts", {}).get(key, 0)

    total_suites = len(SUITES)
    ran_suites = len(results)
    passed_suites = sum(1 for r in results if r["returncode"] == 0)
    failed_suites = [r for r in results if r["returncode"] != 0]
    skipped_suites = total_suites - ran_suites

    total_tests = totals["passed"] + totals["failed"] + totals["error"] + totals["skipped"]

    print(f"\n{'=' * 60}", flush=True)
    print(f"Suites: {ran_suites}/{total_suites} ran, {passed_suites} passed, {len(failed_suites)} failed, {skipped_suites} skipped", flush=True)
    print(f"Tests:  {total_tests} collected, {totals['passed']} passed, {totals['failed']} failed, {totals['error']} error, {totals['skipped']} skipped, {totals['deselected']} deselected", flush=True)
    print(f"{'=' * 60}", flush=True)

    if failed_suites:
        print("failed suites:", flush=True)
        for result in failed_suites:
            print(f"  - {result['name']} ({result['log']})", flush=True)

    summary = {
        "status": "failed" if failed_suites else "passed",
        "suites_total": total_suites,
        "suites_ran": ran_suites,
        "suites_passed": passed_suites,
        "suites_failed": len(failed_suites),
        "suites_skipped": skipped_suites,
        "tests_passed": totals["passed"],
        "tests_failed": totals["failed"],
        "tests_error": totals["error"],
        "tests_skipped": totals["skipped"],
        "tests_deselected": totals["deselected"],
        "results": results,
    }
    Path(args.json_summary_file).write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )
    return 1 if failed_suites else 0

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
