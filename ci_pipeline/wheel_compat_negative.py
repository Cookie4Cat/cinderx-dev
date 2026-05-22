#!/usr/bin/env python3
"""Validate expected CinderX wheel failures on unsupported Python runtimes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any


UNSUPPORTED_INSTALL_PATTERNS = (
    re.compile(r"requires-python", re.IGNORECASE),
    re.compile(r"requires a different python", re.IGNORECASE),
    re.compile(r"not a supported wheel", re.IGNORECASE),
    re.compile(r"is not supported on this platform", re.IGNORECASE),
)


def run_logged(
    args: list[str],
    log_path: Path,
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        args,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    log_path.write_text(
        "$ " + " ".join(args) + "\n\n" + completed.stdout,
        encoding="utf-8",
    )
    return completed


def find_repo_root() -> Path:
    path = Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "cinderx").is_dir():
            return parent
    raise RuntimeError("could not find repository root")


def venv_python(venv_dir: Path) -> Path:
    if sys.platform == "win32":
        return venv_dir / "Scripts" / "python.exe"
    return venv_dir / "bin" / "python"


def read_python_version(
    python: str,
    log_dir: Path,
    repo: Path,
) -> tuple[str | None, int]:
    completed = run_logged(
        [
            python,
            "-c",
            (
                "import sys; "
                "print(f'{sys.version_info.major}.{sys.version_info.minor}."
                "{sys.version_info.micro}')"
            ),
        ],
        log_dir / "python_version.log",
        cwd=repo,
    )
    if completed.returncode != 0:
        return None, completed.returncode
    return completed.stdout.strip().splitlines()[-1], 0


def install_failure_reason_matched(text: str) -> bool:
    return any(pattern.search(text) for pattern in UNSUPPORTED_INSTALL_PATTERNS)


def fallback_import_code(python_lib: Path) -> str:
    return f"""
import json
import sys
import warnings

sys.path.insert(0, {str(python_lib)!r})

result = {{}}
failures = []

try:
    import cinderx
    from cinderx import jit

    def sample():
        return 42

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        enable_result = jit.enable()

    result.update(
        {{
            "imported": True,
            "is_initialized": cinderx.is_initialized(),
            "import_error": repr(cinderx.get_import_error()),
            "jit_is_enabled": jit.is_enabled(),
            "jit_force_compile": jit.force_compile(sample),
            "jit_enable_result": enable_result,
        }}
    )

    if cinderx.is_initialized():
        failures.append("cinderx unexpectedly initialized")
    if cinderx.get_import_error() is None:
        failures.append("missing cinderx import error")
    if jit.is_enabled():
        failures.append("jit unexpectedly enabled")
    if jit.force_compile(sample):
        failures.append("jit unexpectedly force-compiled a function")
except Exception as exc:
    result.update(
        {{
            "imported": False,
            "exception_type": type(exc).__name__,
            "exception": str(exc),
        }}
    )
    failures.append("fallback import raised an exception")

result["status"] = "failed" if failures else "passed"
result["failures"] = failures
print(json.dumps(result, sort_keys=True))
raise SystemExit(1 if failures else 0)
"""


def parse_json_from_output(text: str) -> dict[str, Any] | None:
    for line in reversed(text.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            continue
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", required=True, help="unsupported Python runtime")
    parser.add_argument("--wheel", required=True, help="CinderX wheel to reject")
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--json-summary-file", required=True)
    args = parser.parse_args(argv)

    repo = find_repo_root()
    wheel = Path(args.wheel).expanduser()
    log_dir = Path(args.log_dir)
    summary_path = Path(args.json_summary_file)
    run_dir = summary_path.parent
    venv_dir = run_dir / "venv-unsupported"

    log_dir.mkdir(parents=True, exist_ok=True)
    summary_path.parent.mkdir(parents=True, exist_ok=True)

    summary: dict[str, Any] = {
        "status": "failed",
        "python": args.python,
        "wheel": str(wheel),
        "checks": {},
    }

    if not wheel.exists():
        summary["error"] = f"wheel does not exist: {wheel}"
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        return 1

    python_version, version_rc = read_python_version(args.python, log_dir, repo)
    summary["python_version"] = python_version
    if version_rc != 0:
        summary["error"] = "failed to execute unsupported Python"
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        return 1

    if venv_dir.exists():
        shutil.rmtree(venv_dir)
    venv_result = run_logged(
        [args.python, "-m", "venv", str(venv_dir)],
        log_dir / "create_venv.log",
        cwd=repo,
    )
    summary["checks"]["create_venv"] = {
        "status": "passed" if venv_result.returncode == 0 else "failed",
        "returncode": venv_result.returncode,
        "log": str((log_dir / "create_venv.log").relative_to(repo)),
    }
    if venv_result.returncode != 0:
        summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        return 1

    install_log = log_dir / "install_unsupported.log"
    install_result = run_logged(
        [
            str(venv_python(venv_dir)),
            "-m",
            "pip",
            "install",
            "--force-reinstall",
            str(wheel),
        ],
        install_log,
        cwd=repo,
    )
    install_failed_as_expected = install_result.returncode != 0
    install_reason_matched = install_failure_reason_matched(install_result.stdout)
    summary["checks"]["install_unsupported"] = {
        "status": (
            "passed"
            if install_failed_as_expected and install_reason_matched
            else "failed"
        ),
        "returncode": install_result.returncode,
        "failed_as_expected": install_failed_as_expected,
        "reason_matched": install_reason_matched,
        "log": str(install_log.relative_to(repo)),
    }

    fallback_log = log_dir / "import_fallback.log"
    fallback_result = run_logged(
        [
            args.python,
            "-c",
            fallback_import_code(repo / "cinderx" / "PythonLib"),
        ],
        fallback_log,
        cwd=repo,
    )
    fallback_details = parse_json_from_output(fallback_result.stdout) or {}
    summary["checks"]["import_fallback"] = {
        "status": "passed" if fallback_result.returncode == 0 else "failed",
        "returncode": fallback_result.returncode,
        "details": fallback_details,
        "log": str(fallback_log.relative_to(repo)),
    }

    failed_checks = [
        name
        for name, check in summary["checks"].items()
        if check.get("status") != "passed"
    ]
    summary["status"] = "failed" if failed_checks else "passed"
    summary["failed_checks"] = failed_checks
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    if failed_checks:
        print("failed checks:", ", ".join(failed_checks), flush=True)
        return 1
    print("wheel compatibility negative checks passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
