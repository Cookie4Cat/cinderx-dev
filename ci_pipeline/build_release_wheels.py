#!/usr/bin/env python3
"""Build every release wheel for a tag into one wheelhouse.

The tag-triggered entry point (build_with_tag/gitcode_webhook.py runs this
as CINDERX_BUILD_COMMAND): one release carries the CPython 3.14 manylinux
fat wheel and the CPython 3.11 openEuler wheel, and the webhook's generic
upload loop publishes every wheel it finds in wheelhouse/ with a sha256
sidecar.

Atomic by design: any failing build fails the whole run, so a release
either carries the complete wheel set or nothing.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def env_bool(name: str, default: bool) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.lower() in {"1", "true", "yes", "on"}

# Ordered: the long cp314 build first, the ~15-minute cp311 build after it.
# The order also matters for correctness: the cp314 in-container script
# clears every cinderx wheel from the shared wheelhouse when it starts,
# while the cp311 driver clears only its own cp311-tagged artifacts.
BUILDS = ("cp314", "cp311")


def build_command(name: str, wheelhouse: Path, cp314_build_dir: Path) -> list[str]:
    if name == "cp314":
        # The build dir must live OUTSIDE the checkout: the cp314 container
        # mounts the source read-only at /src and copies it wholesale, so an
        # in-repo output dir (the cp314 driver's own default) sits inside the
        # copy source and trips cp's self-copy detection.
        return [
            sys.executable,
            str(REPO_ROOT / "ci_pipeline" / "build_cp314_manylinux_fat_wheel.py"),
            "--wheelhouse-dir", str(wheelhouse),
            "--output-dir", str(cp314_build_dir),
        ]
    if name == "cp311":
        return [
            sys.executable,
            str(REPO_ROOT / "ci_pipeline" / "build_cp311_wheel.py"),
            "--output-dir", str(wheelhouse),
        ]
    raise ValueError(f"unknown build: {name}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--only",
        action="append",
        choices=BUILDS,
        help="restrict to one build (repeatable); default is all of them",
    )
    parser.add_argument(
        "--wheelhouse-dir",
        default=str(REPO_ROOT / "wheelhouse"),
        help="output directory the webhook uploads from",
    )
    parser.add_argument(
        "--cp314-build-dir",
        default=str(REPO_ROOT.parent / "cinderx-cp314-fat-build"),
        help="cp314 work/logs directory; must not live inside the checkout",
    )
    args = parser.parse_args()

    if args.only:
        selected = tuple(args.only)
    else:
        selected = BUILDS
        # The same switch the webhook honors; an explicit --only overrides.
        if not env_bool("CINDERX_RELEASE_CP311", True):
            selected = tuple(name for name in selected if name != "cp311")

    # One commit epoch for every builder, so both wheels carry the same
    # date-derived version and deterministic zip metadata.  The webhook
    # exports it already; this fills it in for standalone runs.
    if not os.environ.get("SOURCE_DATE_EPOCH"):
        try:
            epoch = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "log", "-1", "--format=%ct"],
                check=True, capture_output=True, text=True,
            ).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            epoch = ""
        if epoch:
            os.environ["SOURCE_DATE_EPOCH"] = epoch

    wheelhouse = Path(args.wheelhouse_dir).resolve()
    wheelhouse.mkdir(parents=True, exist_ok=True)
    cp314_build_dir = Path(args.cp314_build_dir).resolve()

    for name in BUILDS:
        if name not in selected:
            continue
        cmd = build_command(name, wheelhouse, cp314_build_dir)
        print(f"[release-wheels] START {name}: {' '.join(cmd)}", flush=True)
        subprocess.run(cmd, check=True, cwd=str(REPO_ROOT))
        print(f"[release-wheels] DONE {name}", flush=True)

    wheels = sorted(wheelhouse.glob("cinderx-*.whl"))
    if not wheels:
        raise SystemExit(f"no wheels in {wheelhouse}")
    print("[release-wheels] wheelhouse:")
    for wheel in wheels:
        print(f"  {wheel.name}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
