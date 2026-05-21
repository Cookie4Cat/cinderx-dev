#!/usr/bin/env python3
"""Run CPython Lib/test for the local CinderX gate."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import site
import subprocess
import sys
from typing import Iterable


def find_repo_root() -> Path:
    path = Path(__file__).resolve()
    for parent in path.parents:
        if (parent / "CMakeLists.txt").exists() and (parent / "cinderx").is_dir():
            return parent
    raise RuntimeError("could not find repository root")


REPO_ROOT = find_repo_root()
KUNPENG_TESTS_DIR = REPO_ROOT / "cinderx" / "TestScripts" / "TestScriptsKunpeng"
TEST_SCRIPTS_DIR = REPO_ROOT / "cinderx" / "TestScripts"
KUNPENG_DAILY_IGNORE_FILE = KUNPENG_TESTS_DIR / "lib_test_daily_ignore_tests.txt"
MAX_WORKERS = 64
DEFAULT_WORKER_RESPAWN_INTERVAL = 10
DEFAULT_ADAPTIVE_AWARE_COMPILE_AFTER = 24
PROXY_ENV_VARS = (
    "HTTP_PROXY",
    "HTTPS_PROXY",
    "ALL_PROXY",
    "NO_PROXY",
    "http_proxy",
    "https_proxy",
    "all_proxy",
    "no_proxy",
)

FRAME_EVAL_ADAPTIVE_AWARE_MODE = "frame-eval-adaptive-aware"


def read_skip_file(path: Path) -> tuple[set[str], set[str]]:
    modules: set[str] = set()
    patterns: set[str] = set()
    with path.open(encoding="utf-8") as skip_file:
        for raw_line in skip_file:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if "." in line or "*" in line:
                patterns.add(line)
            else:
                modules.add(line)
    return modules, patterns


def is_asan_build() -> bool:
    try:
        from test import support
    except ImportError:
        return False
    return bool(support.check_sanitizer(address=True))


def skip_file_names(*, huntrleaks: bool, use_rr: bool) -> list[str]:
    names = ["devserver_skip_tests.txt", "cinder_skip_test.txt"]

    version = "".join(str(v) for v in sys.version_info[:2])
    versioned_name = f"cinder_skip_test_{version}.txt"
    if (TEST_SCRIPTS_DIR / versioned_name).exists():
        names.append(versioned_name)

    if is_asan_build():
        names.append("asan_skip_tests.txt")

    if use_rr:
        names.append("rr_skip_tests.txt")

    names.append("cinder_jit_ignore_tests.txt")
    names.append(f"cinder_jit_ignore_tests_{version}.txt")

    if huntrleaks:
        names.append("refleak_skip_tests.txt")

    if platform.processor() != "" and platform.processor() != platform.machine():
        names.append("cross_platform_skip_tests.txt")

    return names


def load_skip_metadata(
    *, huntrleaks: bool = False, use_rr: bool = False
) -> tuple[list[str], set[str], set[str]]:
    names = skip_file_names(huntrleaks=huntrleaks, use_rr=use_rr)
    modules: set[str] = set()
    patterns: set[str] = set()
    existing_names: list[str] = []
    for name in names:
        path = TEST_SCRIPTS_DIR / name
        if not path.exists():
            continue
        existing_names.append(name)
        file_modules, file_patterns = read_skip_file(path)
        modules.update(file_modules)
        patterns.update(file_patterns)
    if KUNPENG_DAILY_IGNORE_FILE.exists():
        existing_names.append(str(KUNPENG_DAILY_IGNORE_FILE.relative_to(REPO_ROOT)))
        file_modules, file_patterns = read_skip_file(KUNPENG_DAILY_IGNORE_FILE)
        modules.update(file_modules)
        patterns.update(file_patterns)
    return existing_names, modules, patterns


def discover_lib_tests(exclude: set[str]) -> list[str]:
    from test.libregrtest import findtests as libregrtest_findtests

    tests = libregrtest_findtests.findtests(
        exclude=exclude,
        base_mod="test",
        split_test_dirs={"test." + d for d in libregrtest_findtests.SPLITTESTDIRS},
    )
    return sorted(test for test in tests if test.startswith("test."))


def write_lines(path: Path, lines: Iterable[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{line}\n" for line in lines), encoding="utf-8")


def read_test_file(path: Path) -> list[str]:
    tests = []
    with path.open(encoding="utf-8") as test_file:
        for raw_line in test_file:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            tests.append(line)
    return tests


def default_num_workers() -> int:
    return min(os.cpu_count() or 1, MAX_WORKERS)


def parse_num_workers(value: str | None) -> int:
    value = value or os.environ.get("CINDERX_TESTGATE_WORKERS", "auto")
    if value == "auto":
        return default_num_workers()
    workers = int(value)
    if workers <= 0:
        raise argparse.ArgumentTypeError("--num-workers must be positive or 'auto'")
    return workers


def write_startup_hook(
    path: Path,
    *,
    adaptive_compile_after: int | None = None,
) -> None:
    path.mkdir(parents=True, exist_ok=True)
    hook = [
        "import sys\n",
        "\n",
        "# CPython's regression tests spawn child interpreters to validate clean\n",
        "# startup behavior. Keep this hook on `python -m test` regrtest processes,\n",
        "# but do not alter temporary subprocesses used by those startup tests.\n",
        "argv0 = sys.argv[0].replace('\\\\', '/') if sys.argv else ''\n",
        "orig_argv = tuple(getattr(sys, 'orig_argv', ()))\n",
        "module_name = None\n",
        "if '-m' in orig_argv:\n",
        "    module_index = orig_argv.index('-m') + 1\n",
        "    if module_index < len(orig_argv):\n",
        "        module_name = orig_argv[module_index]\n",
        "if (\n",
        "    module_name in {'test', 'test.libregrtest.worker'}\n",
        "    or argv0.endswith('/test/__main__.py')\n",
        "    or argv0.endswith('/test/regrtest.py')\n",
        "    or argv0.endswith('/cinderx/TestScripts/TestScriptsKunpeng/lib_test_dispatcher.py')\n",
        "):\n",
        "    import os\n",
    ]
    hook.extend(
        [
        "    import cinderx\n",
        "    cinderx.init()\n",
        "    if not cinderx.is_initialized():\n",
        "        raise RuntimeError('CinderX failed to initialize')\n",
        "    try:\n",
        "        import _testcapi\n",
        "        import types\n",
        "        original_get_code = _testcapi.gen_get_code\n",
        "        def gen_get_code(o):\n",
        "            if type(o) is not types.GeneratorType:\n",
        "                return o.gi_code\n",
        "            return original_get_code(o)\n",
        "        _testcapi.gen_get_code = gen_get_code\n",
        "        original_raise_sigint_then_send_none = getattr(\n",
        "            _testcapi, 'raise_SIGINT_then_send_None', None)\n",
        "        if original_raise_sigint_then_send_none is not None:\n",
        "            import cinderx.jit\n",
        "            def raise_SIGINT_then_send_None(o):\n",
        "                cinderx.jit._deopt_gen(o)\n",
        "                return original_raise_sigint_then_send_none(o)\n",
        "            _testcapi.raise_SIGINT_then_send_None = (\n",
        "                raise_SIGINT_then_send_None)\n",
        "    except (ImportError, AttributeError):\n",
        "        pass\n",
        ]
    )
    hook.extend(
        [
            "    if not cinderx.is_frame_evaluator_installed():\n",
            "        cinderx.install_frame_evaluator()\n",
            "    if not cinderx.is_frame_evaluator_installed():\n",
            "        raise RuntimeError('CinderX frame evaluator is not installed')\n",
            "    import cinderx.jit\n",
            "    if not cinderx.jit.is_enabled():\n",
            "        raise RuntimeError('CinderX JIT is not enabled')\n",
            "    adaptive_compile_after = os.environ.get(\n",
            "        'CINDERX_TESTGATE_ADAPTIVE_COMPILE_AFTER')\n",
            "    if adaptive_compile_after is not None:\n",
            "        adaptive_compile_after = int(adaptive_compile_after)\n",
            "        cinderx.jit.compile_after_n_calls(adaptive_compile_after)\n",
            "        if cinderx.jit.get_compile_after_n_calls() != adaptive_compile_after:\n",
            "            raise RuntimeError('CinderX JIT adaptive-aware threshold mismatch')\n",
        ]
    )
    hook.extend(
        [
            "    marker = os.environ.get('CINDERX_TESTGATE_FRAME_EVAL_MARKER')\n",
            "    if marker:\n",
            "        with open(marker, 'a', encoding='utf-8') as marker_file:\n",
            "            marker_file.write(f'{os.getpid()} {argv0}\\n')\n",
        ]
    )
    (path / "sitecustomize.py").write_text("".join(hook), encoding="utf-8")


def env_for_mode(startup_dir: Path, marker_file: Path) -> dict[str, str]:
    env = os.environ.copy()
    for name in PROXY_ENV_VARS:
        env.pop(name, None)

    # Isolate stdlib test temp files per gate run so concurrent wheel_compat
    # variants do not race on shared names under /tmp (for example test_filecmp).
    temp_root = startup_dir.parent / f"{startup_dir.stem}_tmp"
    temp_root.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(temp_root)
    env["TEMP"] = str(temp_root)
    env["TMP"] = str(temp_root)

    site_packages = site.getsitepackages()[0]
    pythonpath_entries = [str(startup_dir), site_packages]
    if pythonpath := env.get("PYTHONPATH"):
        pythonpath_entries.append(pythonpath)
    env["PYTHONPATH"] = os.pathsep.join(pythonpath_entries)
    env["CINDERX_TESTGATE_FRAME_EVAL_MARKER"] = str(marker_file)

    openssl_lib = env.get("CINDERX_TEST_OPENSSL_LIB")
    if not openssl_lib:
        candidate = Path("/opt/openssl-1.1.1w-vanilla/lib")
        if (candidate / "libssl.so.1.1").exists() and (
            candidate / "libcrypto.so.1.1"
        ).exists():
            openssl_lib = str(candidate)
    if openssl_lib:
        ld_library_path = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = (
            openssl_lib
            if not ld_library_path
            else f"{openssl_lib}{os.pathsep}{ld_library_path}"
        )

    env.pop("CINDERX_JIT_DISABLE", None)
    env.pop("PYTHONJITDISABLE", None)

    return env


def adaptive_compile_after_for_mode(mode: str, value: int | None) -> int | None:
    if mode != FRAME_EVAL_ADAPTIVE_AWARE_MODE:
        return None
    if value is None:
        value = DEFAULT_ADAPTIVE_AWARE_COMPILE_AFTER
    if value <= 0:
        raise argparse.ArgumentTypeError(
            "--adaptive-compile-after must be positive"
        )
    return value


def normalize_tests(tests: list[str] | None) -> list[str] | None:
    if tests is None:
        return None
    normalized = []
    for test in tests:
        if test.startswith("test_"):
            normalized.append(f"test.{test}")
        else:
            normalized.append(test)
    return normalized


def regrtest_command(
    *,
    tests: list[str],
    ignore_file: Path,
    num_workers: int,
    worker_timeout: int,
    single_process: bool = False,
) -> list[str]:
    command = [
        sys.executable,
        "-m",
        "test",
        "-q",
        "--timeout",
        str(worker_timeout),
        "--fail-env-changed",
        "--fail-rerun",
        "-w",
    ]
    if single_process:
        command.append("--single-process")
    else:
        command.extend(["-j", str(num_workers)])
    command.extend(tests)
    if ignore_file.exists() and ignore_file.stat().st_size:
        command.extend(["--ignorefile", str(ignore_file)])
    return command


def dispatcher_command(
    *,
    test_list_file: Path,
    ignore_file: Path,
    json_summary_file: Path,
    num_workers: int,
    worker_timeout: int,
    worker_respawn_interval: int,
) -> list[str]:
    command = [
        sys.executable,
        str(KUNPENG_TESTS_DIR / "lib_test_dispatcher.py"),
        "dispatcher",
        "--test-list-file",
        str(test_list_file),
        "--json-summary-file",
        str(json_summary_file),
        "--num-workers",
        str(num_workers),
        "--worker-timeout",
        str(worker_timeout),
        "--worker-respawn-interval",
        str(worker_respawn_interval),
    ]
    if ignore_file.exists() and ignore_file.stat().st_size:
        command.extend(["--ignorefile", str(ignore_file)])
    return command


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mode",
        choices=[FRAME_EVAL_ADAPTIVE_AWARE_MODE],
        default=FRAME_EVAL_ADAPTIVE_AWARE_MODE,
        help="Lib/test execution mode",
    )
    parser.add_argument("--json-summary-file", required=True)
    parser.add_argument("--test-list-file", required=True)
    parser.add_argument(
        "--test-from-file",
        help="read tests from this file instead of discovering Lib/test",
    )
    parser.add_argument(
        "--include-skipped-modules",
        action="store_true",
        help=(
            "when --test or --test-from-file is used, keep tests that are "
            "listed in module-level skip metadata"
        ),
    )
    parser.add_argument("--num-workers", default=None)
    parser.add_argument("--worker-timeout", type=int, default=20 * 60)
    parser.add_argument(
        "--worker-respawn-interval",
        type=int,
        default=DEFAULT_WORKER_RESPAWN_INTERVAL,
        help="number of Lib/test modules to run in each dispatcher worker",
    )
    parser.add_argument(
        "--runner",
        choices=["regrtest", "dispatcher"],
        default="regrtest",
        help="Lib/test execution backend",
    )
    parser.add_argument(
        "--adaptive-compile-after",
        type=int,
        default=DEFAULT_ADAPTIVE_AWARE_COMPILE_AFTER,
        help=(
            "AutoJIT threshold for frame-eval-adaptive-aware "
            f"(default: {DEFAULT_ADAPTIVE_AWARE_COMPILE_AFTER})"
        ),
    )
    parser.add_argument(
        "-t",
        "--test",
        action="append",
        help="run only these tests instead of the official Lib/test list",
    )
    args = parser.parse_args(argv)

    try:
        num_workers = parse_num_workers(args.num_workers)
    except (argparse.ArgumentTypeError, ValueError) as exc:
        parser.error(f"invalid --num-workers value: {exc}")
    try:
        adaptive_compile_after = adaptive_compile_after_for_mode(
            args.mode, args.adaptive_compile_after
        )
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))

    skip_files, skip_modules, skip_patterns = load_skip_metadata()
    tests = normalize_tests(args.test)
    explicit_tests = tests is not None or args.test_from_file is not None
    if args.include_skipped_modules and not explicit_tests:
        parser.error("--include-skipped-modules requires --test or --test-from-file")
    if tests is None and args.test_from_file:
        tests = normalize_tests(read_test_file(Path(args.test_from_file)))
    if tests is None:
        tests = discover_lib_tests(skip_modules)
    if not args.include_skipped_modules:
        tests = [test for test in tests if test not in skip_modules]

    test_list_path = Path(args.test_list_file).resolve()
    write_lines(test_list_path, tests)

    ignore_file = test_list_path.with_name(f"{test_list_path.stem}_ignore_patterns.txt")
    write_lines(ignore_file, sorted(skip_patterns))

    single_process_tests: list[str] = []
    parallel_tests = tests
    parallel_test_list_path = test_list_path.with_name(
        f"{test_list_path.stem}_parallel.txt"
    )
    write_lines(parallel_test_list_path, parallel_tests)

    startup_dir = test_list_path.with_name(f"{test_list_path.stem}_startup")
    write_startup_hook(
        startup_dir,
        adaptive_compile_after=adaptive_compile_after,
    )
    marker_file = startup_dir / "frame_eval_startups.txt"

    env = env_for_mode(startup_dir, marker_file)
    if adaptive_compile_after is not None:
        env["CINDERX_TESTGATE_ADAPTIVE_COMPILE_AFTER"] = str(
            adaptive_compile_after
        )
    commands: list[list[str]] = []
    dispatcher_summary_path = None
    if parallel_tests:
        if args.runner == "dispatcher":
            dispatcher_summary_path = test_list_path.with_name(
                f"{test_list_path.stem}_dispatcher.json"
            )
            commands.append(
                dispatcher_command(
                    test_list_file=parallel_test_list_path,
                    ignore_file=ignore_file,
                    json_summary_file=dispatcher_summary_path,
                    num_workers=num_workers,
                    worker_timeout=args.worker_timeout,
                    worker_respawn_interval=args.worker_respawn_interval,
                )
            )
        else:
            commands.append(
                regrtest_command(
                    tests=["--fromfile", str(parallel_test_list_path)],
                    ignore_file=ignore_file,
                    num_workers=num_workers,
                    worker_timeout=args.worker_timeout,
                )
            )
    for test in single_process_tests:
        commands.append(
            regrtest_command(
                tests=[test],
                ignore_file=ignore_file,
                num_workers=num_workers,
                worker_timeout=args.worker_timeout,
                single_process=True,
            )
        )

    returncode = 0
    for command in commands:
        completed = subprocess.run(command, cwd=REPO_ROOT, env=env)
        if completed.returncode != 0:
            returncode = completed.returncode
            break

    if not marker_file.exists():
        print("CinderX frame evaluator startup hook did not run", file=sys.stderr)
        returncode = 1

    summary = {
        "mode": args.mode,
        "returncode": returncode,
        "runner": args.runner,
        "requires_cinderx_frame_evaluator": True,
        "requires_jit_enabled": True,
        "requires_adaptive_aware": True,
        "adaptive_compile_after": adaptive_compile_after,
        "num_workers": num_workers,
        "worker_respawn_interval": (
            args.worker_respawn_interval if args.runner == "dispatcher" else None
        ),
        "dispatcher_summary_file": (
            str(dispatcher_summary_path)
            if dispatcher_summary_path is not None
            else None
        ),
        "startup_hook": str(startup_dir / "sitecustomize.py"),
        "startup_marker": str(marker_file),
        "proxy_env_unset": list(PROXY_ENV_VARS),
        "test_count": len(tests),
        "tests": tests,
        "include_skipped_modules": args.include_skipped_modules,
        "parallel_test_count": len(parallel_tests),
        "parallel_test_list_file": str(parallel_test_list_path),
        "single_process_tests": single_process_tests,
        "skip_files": skip_files,
        "skip_modules": sorted(skip_modules),
        "skip_patterns": sorted(skip_patterns),
        "ignore_patterns_file": str(ignore_file),
        "commands": commands,
    }
    json_summary_file = Path(args.json_summary_file).resolve()
    json_summary_file.parent.mkdir(parents=True, exist_ok=True)
    json_summary_file.write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8"
    )

    return returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
