#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail
set -x

stage() {
  local message="$*"
  echo "[cp314-fat-wheel] ${message} $(date '+%F %T %z')"
  echo "${message}" > /logs/build.stage
}

export PATH="/opt/gcc-14.2/bin:/opt/gcc-14.2.0/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/opt/llvm19/bin:/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/bin"
export LD_LIBRARY_PATH="/opt/rh/gcc-toolset-14/root/usr/lib64:/opt/rh/gcc-toolset-14/root/usr/lib:/opt/rh/gcc-toolset-14/root/usr/lib64/dyninst:/opt/rh/gcc-toolset-14/root/usr/lib/dyninst:${LD_LIBRARY_PATH:-}"
export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1

if [ -x /opt/gcc-14.2/bin/gcc ]; then
  export CC=/opt/gcc-14.2/bin/gcc
  export CXX=/opt/gcc-14.2/bin/g++
elif [ -x /opt/gcc-14.2.0/bin/gcc ]; then
  export CC=/opt/gcc-14.2.0/bin/gcc
  export CXX=/opt/gcc-14.2.0/bin/g++
else
  export CC="${CC:-gcc}"
  export CXX="${CXX:-g++}"
fi

if [ "${CINDERX_ENABLE_PGO:-0}" != "0" ]; then
  export CINDERX_ENABLE_PGO=1
else
  unset CINDERX_ENABLE_PGO
fi

if [ "${CINDERX_ENABLE_LTO:-0}" != "0" ]; then
  export CINDERX_ENABLE_LTO=1
else
  unset CINDERX_ENABLE_LTO
fi

export CINDERX_VERSION_PATCH="${CINDERX_VERSION_PATCH:-0}"
export CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
export CINDERX_REQUIRES_PYTHON="${CINDERX_REQUIRES_PYTHON:->=3.14,<3.14.4}"

cd /
stage "START preflight"
if [ "${CINDERX_SKIP_BUILDER_CHECK:-0}" != "1" ]; then
  command -v check-cpython-314-builds
  check-cpython-314-builds
fi
command -v "$CC"
command -v "$CXX"
command -v cmake
command -v auditwheel
"$CC" --version | head -n 1
cmake --version | head -n 1
auditwheel --version

if [ "${CINDERX_RESUME:-0}" != "1" ]; then
  for d in /ordinary /repaired /work; do
    find "$d" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
  done
  find /fat-wheel -maxdepth 1 -type f -name 'cinderx-*.whl' -delete
fi

ensure_build_deps() {
  local py="$1"
  if "$py" - <<'PY'
import importlib.metadata as metadata


def version_tuple(value):
    out = []
    for part in value.replace("-", ".").split("."):
        if part.isdigit():
            out.append(int(part))
        else:
            break
    return tuple(out)


try:
    setuptools_version = metadata.version("setuptools")
    wheel_version = metadata.version("wheel")
except metadata.PackageNotFoundError:
    raise SystemExit(1)

if version_tuple(setuptools_version) < (77, 0, 3):
    raise SystemExit(1)
print(f"build deps ok: setuptools={setuptools_version} wheel={wheel_version}")
PY
  then
    return 0
  fi
  "$py" -m pip install --disable-pip-version-check --no-cache-dir 'setuptools>=77.0.3' wheel
}

snapshot_python() {
  local ver="$1"
  local py="$2"
  CINDERX_SNAPSHOT_VERSION="$ver" "$py" - <<'PY' >> /logs/cpython-builds.jsonl
import json
import os
import sys
import sysconfig

keys = [
    "CONFIG_ARGS",
    "EXT_SUFFIX",
    "Py_DEBUG",
    "Py_GIL_DISABLED",
    "Py_TAIL_CALL_INTERP",
    "Py_TRACE_REFS",
    "SOABI",
]
print(json.dumps({
    "version": os.environ["CINDERX_SNAPSHOT_VERSION"],
    "executable": sys.executable,
    "sys_version": sys.version,
    "config": {key: sysconfig.get_config_var(key) for key in keys},
}, sort_keys=True))
PY
}

stage "START python-build-dep-check"
: > /logs/cpython-builds.jsonl
for ver in 3.14.0 3.14.1 3.14.2 3.14.3; do
  py="/opt/cpython/${ver}/bin/python3.14"
  test -x "$py"
  ensure_build_deps "$py"
  "$py" -m pip --version
  snapshot_python "$ver" "$py"
done

build_one() {
  local ver="$1"
  local variant="py314${ver##*.}"
  local py="/opt/cpython/${ver}/bin/python3.14"
  local work="/work/${variant}/src"

  stage "START ordinary ${variant} ${ver}"
  mkdir -p "/work/${variant}" "/ordinary/${variant}" "/repaired/${variant}"
  rm -rf "$work"
  mkdir -p "$work"
  cp -a /src/. "$work/"
  cd "$work"
  rm -rf scratch build dist ./*.egg-info
  "$py" -m pip wheel --no-build-isolation --no-deps --no-cache-dir -w "/ordinary/${variant}" .

  local ordinary
  ordinary=$(find "/ordinary/${variant}" -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
  test -n "$ordinary"
  echo "[cp314-fat-wheel] BUILT ordinary ${variant} ${ordinary}"
  sha256sum "$ordinary" | tee "/logs/ordinary-${variant}.sha256"

  stage "START auditwheel-show ${variant}"
  auditwheel show "$ordinary" 2>&1 | tee "/logs/auditwheel-show-${variant}.log"

  stage "START auditwheel-repair ${variant}"
  auditwheel repair --plat manylinux_2_28_aarch64 -w "/repaired/${variant}" "$ordinary" 2>&1 | tee "/logs/auditwheel-repair-${variant}.log"

  local repaired
  repaired=$(find "/repaired/${variant}" -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
  test -n "$repaired"
  echo "[cp314-fat-wheel] REPAIRED ${variant} ${repaired}"
  sha256sum "$repaired" | tee "/logs/repaired-${variant}.sha256"
  stage "DONE ${variant}"
}

build_one 3.14.0
build_one 3.14.1
build_one 3.14.2
build_one 3.14.3

py3140=$(find /repaired/py3140 -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
py3141=$(find /repaired/py3141 -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
py3142=$(find /repaired/py3142 -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
py3143=$(find /repaired/py3143 -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
for wheel in "$py3140" "$py3141" "$py3142" "$py3143"; do
  test -n "$wheel"
  test -f "$wheel"
done

output="/fat-wheel/$(basename "$py3143")"
stage "START fat-repack ${output}"
/opt/cpython/3.14.3/bin/python3.14 /src/ci_pipeline/scripts/build_cp314_fat_wheel.py \
  --py3140-wheel "$py3140" \
  --py3141-wheel "$py3141" \
  --py3142-wheel "$py3142" \
  --py3143-wheel "$py3143" \
  --output-wheel "$output" \
  --requires-python "$CINDERX_REQUIRES_PYTHON" \
  --force

echo "[cp314-fat-wheel] FAT ${output}"
sha256sum "$output" | tee /logs/fat-wheel.sha256

/opt/cpython/3.14.3/bin/python3.14 - <<'PY'
from pathlib import Path
from zipfile import ZipFile
import json

wheel = sorted(Path("/fat-wheel").glob("cinderx-*.whl"))[-1]
with ZipFile(wheel) as zf:
    names = sorted(zf.namelist())
    top_native = [
        name for name in names
        if "/" not in name and name.startswith("_cinderx") and name.endswith(".so")
    ]
    native = [
        name for name in names
        if name.startswith("cinderx/_native/") and name.endswith(".so")
    ]
    metadata_name = next(name for name in names if name.endswith(".dist-info/METADATA"))
    metadata = zf.read(metadata_name).decode()
    requires_python = [
        line for line in metadata.splitlines() if line.startswith("Requires-Python:")
    ]
    inspect = {
        "wheel": str(wheel),
        "top_level_native_so": top_native,
        "has_loader": "_cinderx.py" in names,
        "native_variants": native,
        "requires_python": requires_python,
    }
    print(json.dumps(inspect, indent=2, sort_keys=True))
    Path("/logs/fat-wheel-inspect.json").write_text(json.dumps(inspect, indent=2, sort_keys=True) + "\n")
    if top_native:
        raise SystemExit(f"fat wheel must not contain top-level native _cinderx*.so: {top_native}")
    if "_cinderx.py" not in names:
        raise SystemExit("fat wheel is missing _cinderx.py loader")
    expected_dirs = {"py314_0", "py314_1", "py314_2", "py314_3"}
    actual_dirs = {Path(name).parts[2] for name in native}
    if actual_dirs != expected_dirs:
        raise SystemExit(f"native variant directories mismatch: {sorted(actual_dirs)}")
    if not requires_python:
        raise SystemExit("fat wheel is missing Requires-Python metadata")
PY

if [ "${CINDERX_SKIP_SMOKE:-0}" != "1" ]; then
  stage "START smoke"
  fat=$(find /fat-wheel -maxdepth 1 -type f -name 'cinderx-*.whl' | sort | tail -n 1)
  test -n "$fat"
  : > /logs/fat-wheel-smoke.log
  for ver in 3.14.0 3.14.1 3.14.2 3.14.3; do
    py="/opt/cpython/${ver}/bin/python3.14"
    venv="/tmp/cinderx-fat-smoke-${ver}"
    rm -rf "$venv"
    "$py" -m venv "$venv"
    "$venv/bin/python" -m pip install --no-index --no-deps "$fat"
    "$venv/bin/python" - <<'PY' | tee -a /logs/fat-wheel-smoke.log
import sys
import cinderx
import _cinderx

print("smoke", sys.version.split()[0], cinderx.is_initialized(), getattr(_cinderx, "__file__", None))
assert sys.version_info[:2] == (3, 14)
assert cinderx.is_initialized() is not None
assert cinderx.is_lightweight_frames_enabled()
PY
  done
fi

stage "DONE fat-repack"
