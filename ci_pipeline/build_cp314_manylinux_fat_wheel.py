#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
"""Build a CPython 3.14.0-3.14.3 manylinux fat CinderX wheel.

This script orchestrates the productized fat-wheel packaging flow:

1. run a pinned CinderX CPython 3.14 builder image;
2. build four ordinary cp314-cp314 wheels with CPython 3.14.0/1/2/3;
3. repair each ordinary wheel with auditwheel;
4. repack the four repaired wheels with scripts/build_cp314_fat_wheel.py;
5. write the final fat wheel to the project wheelhouse directory;
6. inspect and optionally smoke-test the resulting fat wheel.

The builder image is expected to provide:

* /opt/cpython/3.14.0/bin/python3.14
* /opt/cpython/3.14.1/bin/python3.14
* /opt/cpython/3.14.2/bin/python3.14
* /opt/cpython/3.14.3/bin/python3.14
* auditwheel
* /usr/local/bin/check-cpython-314-builds
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


DEFAULT_IMAGE = "cinderx-cp314-builder:manylinux_2_28_aarch64-20260519"
DEFAULT_REQUIRES_PYTHON = ">=3.14,<3.14.4"


class BuildError(Exception):
    pass


def shlex_join(args: list[str]) -> str:
    import shlex

    return shlex.join(args)


def run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    print("+", shlex_join(cmd), flush=True)
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd is not None else None,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def maybe_run(
    cmd: list[str],
    *,
    cwd: Path | None = None,
) -> str | None:
    try:
        completed = run(cmd, cwd=cwd, capture=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[1]


def validate_source_dir(source_dir: Path) -> None:
    required = (
        "pyproject.toml",
        "setup.py",
        "ci_pipeline/scripts/build_cp314_fat_wheel.py",
        "ci_pipeline/scripts/build_cp314_manylinux_fat_wheel_in_container.sh",
    )
    missing = [name for name in required if not (source_dir / name).is_file()]
    if missing:
        raise BuildError(
            f"{source_dir} does not look like a CinderX source tree; "
            f"missing {', '.join(missing)}"
        )


def git_metadata(source_dir: Path) -> dict[str, object]:
    head = maybe_run(["git", "rev-parse", "HEAD"], cwd=source_dir)
    branch = maybe_run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=source_dir)
    status = maybe_run(["git", "status", "--short"], cwd=source_dir)
    diff_stat = maybe_run(["git", "diff", "--stat"], cwd=source_dir)
    return {
        "head": head,
        "branch": branch,
        "dirty": bool(status),
        "status_short": status.splitlines() if status else [],
        "diff_stat": diff_stat.splitlines() if diff_stat else [],
    }


def prepare_output_dirs(output_dir: Path, wheelhouse_dir: Path, *, clean: bool) -> dict[str, Path]:
    dirs = {
        "ordinary": output_dir / "ordinary",
        "repaired": output_dir / "repaired",
        "fat": wheelhouse_dir,
        "logs": output_dir / "logs",
        "work": output_dir / "work",
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    for key in ("ordinary", "repaired", "logs", "work"):
        path = dirs[key]
        if clean and path.exists():
            shutil.rmtree(path)
    for path in dirs.values():
        path.mkdir(parents=True, exist_ok=True)
    return dirs


def detect_proxy(args: argparse.Namespace) -> str | None:
    if args.proxy:
        return args.proxy
    if args.no_proxy_from_env:
        return None
    for name in ("HTTPS_PROXY", "HTTP_PROXY", "ALL_PROXY", "https_proxy", "http_proxy", "all_proxy"):
        value = os.environ.get(name)
        if value:
            return value
    return None


def docker_env_args(env: dict[str, str | None]) -> list[str]:
    args: list[str] = []
    for key, value in env.items():
        if value is not None:
            args.extend(["-e", f"{key}={value}"])
    return args


def docker_mount_args(mounts: dict[Path, str]) -> list[str]:
    args: list[str] = []
    for host, container in mounts.items():
        args.extend(["-v", f"{host.resolve()}:{container}"])
    return args


def build_docker_command(args: argparse.Namespace, dirs: dict[str, Path]) -> list[str]:
    proxy = detect_proxy(args)
    env: dict[str, str | None] = {
        "CINDERX_ENABLE_PGO": "1" if args.pgo else "0",
        "CINDERX_ENABLE_LTO": "1" if args.lto else "0",
        "CINDERX_VERSION_PATCH": args.version_patch,
        "CINDERX_REQUIRES_PYTHON": args.requires_python,
        "CINDERX_SKIP_SMOKE": "1" if args.skip_smoke else "0",
        "CINDERX_SKIP_BUILDER_CHECK": "1" if args.skip_builder_check else "0",
        "CINDERX_RESUME": "1" if args.resume else "0",
        "CMAKE_BUILD_TYPE": args.cmake_build_type,
        "CMAKE_BUILD_PARALLEL_LEVEL": args.cmake_build_parallel_level,
        "MAKEFLAGS": args.makeflags,
        "HTTP_PROXY": proxy,
        "HTTPS_PROXY": proxy,
        "ALL_PROXY": proxy,
        "http_proxy": proxy,
        "https_proxy": proxy,
        "all_proxy": proxy,
    }

    command = ["docker", "run", "--rm"]
    if args.platform:
        command.extend(["--platform", args.platform])
    if args.docker_network:
        command.extend(["--network", args.docker_network])
    command += docker_env_args(env)
    command += docker_mount_args(
        {
            args.source_dir: "/src:ro",
            dirs["ordinary"]: "/ordinary",
            dirs["repaired"]: "/repaired",
            dirs["fat"]: "/fat-wheel",
            dirs["logs"]: "/logs",
            dirs["work"]: "/work",
        }
    )
    command.extend(
        [
            args.image,
            "/bin/bash",
            "/src/ci_pipeline/scripts/build_cp314_manylinux_fat_wheel_in_container.sh",
        ]
    )
    return command


def write_host_manifest(args: argparse.Namespace, dirs: dict[str, Path], docker_command: list[str]) -> None:
    image_inspect = maybe_run(["docker", "image", "inspect", args.image])
    manifest = {
        "image": args.image,
        "image_inspect": json.loads(image_inspect) if image_inspect else None,
        "source_dir": str(args.source_dir.resolve()),
        "output_dir": str(args.output_dir.resolve()),
        "wheelhouse_dir": str(args.wheelhouse_dir.resolve()),
        "git": git_metadata(args.source_dir),
        "docker_command": docker_command,
        "requires_python": args.requires_python,
        "version_patch": args.version_patch,
        "cmake_build_type": args.cmake_build_type,
        "pgo_enabled": args.pgo,
        "lto_enabled": args.lto,
    }
    (dirs["logs"] / "host-build-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def find_fat_wheel(fat_dir: Path) -> Path:
    wheels = sorted(fat_dir.glob("cinderx-*.whl"))
    if not wheels:
        raise BuildError(f"no fat wheel found in {fat_dir}")
    return wheels[-1]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=repo_root_from_script(),
        help="CinderX source tree to mount read-only into the builder container",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=repo_root_from_script() / "build" / "cp314-fat-wheel",
        help="directory for ordinary wheels, repaired wheels, logs, and work files",
    )
    parser.add_argument(
        "--wheelhouse-dir",
        type=Path,
        help="directory for the final fat wheel; defaults to SOURCE_DIR/wheelhouse",
    )
    parser.add_argument(
        "--image",
        default=os.environ.get("CINDERX_CP314_BUILDER_IMAGE", DEFAULT_IMAGE),
        help="builder Docker image tag or digest",
    )
    parser.add_argument("--platform", help="optional Docker platform, for example linux/arm64")
    parser.add_argument("--docker-network", default="host", help="Docker network mode")
    parser.add_argument("--proxy", help="proxy URL to pass as HTTP(S)/ALL_PROXY")
    parser.add_argument(
        "--no-proxy-from-env",
        action="store_true",
        help="do not pass proxy variables from the host environment",
    )
    parser.add_argument(
        "--version-patch",
        default=os.environ.get("CINDERX_VERSION_PATCH", "0"),
        help="CINDERX_VERSION_PATCH value used by setup.py",
    )
    parser.add_argument("--requires-python", default=DEFAULT_REQUIRES_PYTHON)
    parser.add_argument(
        "--cmake-build-type",
        default="Release",
        help=(
            "CMAKE_BUILD_TYPE used by setup.py inside the builder container; "
            "defaults to Release to avoid shipping RelWithDebInfo debug info"
        ),
    )
    parser.add_argument("--cmake-build-parallel-level")
    parser.add_argument("--makeflags")
    pgo_group = parser.add_mutually_exclusive_group()
    pgo_group.add_argument(
        "--pgo",
        dest="pgo",
        action="store_true",
        default=False,
        help="set CINDERX_ENABLE_PGO=1; disabled by default",
    )
    pgo_group.add_argument(
        "--no-pgo",
        dest="pgo",
        action="store_false",
        help="do not set CINDERX_ENABLE_PGO=1; default",
    )
    lto_group = parser.add_mutually_exclusive_group()
    lto_group.add_argument(
        "--lto",
        dest="lto",
        action="store_true",
        default=False,
        help="set CINDERX_ENABLE_LTO=1; disabled by default",
    )
    lto_group.add_argument(
        "--no-lto",
        dest="lto",
        action="store_false",
        help="do not set CINDERX_ENABLE_LTO=1; default",
    )
    parser.add_argument("--skip-smoke", action="store_true", help="skip import smoke tests inside the builder image")
    parser.add_argument(
        "--skip-builder-check",
        action="store_true",
        help="skip /usr/local/bin/check-cpython-314-builds",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="do not clean output ordinary/repaired/work directories or cinderx wheels in wheelhouse before building",
    )
    parser.add_argument("--dry-run", action="store_true", help="write manifest and print Docker command only")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.source_dir = args.source_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    args.wheelhouse_dir = (
        args.wheelhouse_dir.resolve()
        if args.wheelhouse_dir is not None
        else args.source_dir / "wheelhouse"
    )

    try:
        validate_source_dir(args.source_dir)
        dirs = prepare_output_dirs(args.output_dir, args.wheelhouse_dir, clean=not args.resume)
        container_script = (
            args.source_dir / "ci_pipeline/scripts/build_cp314_manylinux_fat_wheel_in_container.sh"
        )
        docker_command = build_docker_command(args, dirs)
        write_host_manifest(args, dirs, docker_command)

        print(f"source: {args.source_dir}")
        print(f"output: {args.output_dir}")
        print(f"wheelhouse: {args.wheelhouse_dir}")
        print(f"image: {args.image}")
        print(f"container script: {container_script}")
        print("+", shlex_join(docker_command), flush=True)

        if args.dry_run:
            return 0

        run(docker_command)
        fat_wheel = find_fat_wheel(dirs["fat"])
        digest = sha256_file(fat_wheel)
        print(f"fat wheel: {fat_wheel}")
        print(f"sha256: {digest}")
    except (BuildError, subprocess.CalledProcessError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
