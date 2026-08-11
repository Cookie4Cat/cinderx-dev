#!/usr/bin/env bash
# Builder preflight for the cp311 release wheel -- the 3.11 counterpart of
# check-cpython-314-builds.  Refuses to build unless the anchored toolchain
# is actually present: distro CPython 3.11.6, GCC 14, cmake, a setuptools
# new enough for the pyproject license metadata, and the static libstdc++
# archive the self-contained link needs.
set -Eeuo pipefail

# Narrowest-version phase: the exact anchored distro build, compared whole
# (a substring match would wave a "-34.oe2403sp3.1" rebuild through).
PYTHON3_NVR="${CINDERX_PYTHON3_NVR:-3.11.6-34.oe2403sp3}"
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3)" = "python3-${PYTHON3_NVR}"
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3-devel)" = "python3-devel-${PYTHON3_NVR}"
python3.11 -c "import sys; assert sys.version_info[:3] == (3, 11, 6), sys.version"

gcc_version=$("${CXX:-g++}" -dumpfullversion)
case "$gcc_version" in
  14.*) echo "g++ ${gcc_version}" ;;
  *) echo "expected GCC 14.x, got ${gcc_version}" >&2; exit 1 ;;
esac

cmake --version | head -n 1

# Actually link once, with the release link mode: version strings alone
# missed a toolset packaging gap where the compiler installed without its
# own libgcc_s (the gcc rpm does not require gcc-toolset-14-libgcc).
probe=$(mktemp -d)
echo 'int main() { return 0; }' > "${probe}/probe.cc"
"${CXX:-g++}" -static-libstdc++ "${probe}/probe.cc" -o "${probe}/probe"
"${probe}/probe"
rm -rf "$probe"

python3.11 - <<'PY'
import importlib.metadata as metadata

version = metadata.version("setuptools")
major = int(version.split(".", 1)[0])
assert major >= 77, f"setuptools too old for pyproject license metadata: {version}"
print(f"setuptools {version}")
PY

# -static-libstdc++ silently degrades to dynamic linking when the archive
# is missing; assert it exists instead of finding out in the smoke.
test -n "$(find /opt/openEuler/gcc-toolset-14 -name libstdc++.a -print -quit)"

echo "[check-cpython-311-build] OK"
