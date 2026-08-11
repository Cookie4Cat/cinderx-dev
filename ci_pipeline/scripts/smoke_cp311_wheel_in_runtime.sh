#!/usr/bin/env bash
# Prove the cp311 wheel runs on a STOCK openEuler 24.03-LTS-SP3 image.
#
# The runtime container carries the distro python3 rpm plus pip and nothing
# else -- no compiler, no dev headers, none of the dev image's toolchain
# paths.  The repo is not mounted either: only the test_cinderx test package
# is, so every cinderx import in the kunpeng unittests resolves from the
# installed wheel rather than a source checkout.
#
# Mounts (provided by ci_pipeline/build_cp311_wheel.py):
#   /wheels             wheel directory, read-only
#   /smoke/run.sh       this script
#   /smoke/test_cinderx the unittest package, read-only
set -Eeuo pipefail

export PIP_DISABLE_PIP_VERSION_CHECK=1
export PYTHONUNBUFFERED=1

# Narrowest-version phase: the smoke runs on exactly the anchored
# interpreter build, not whatever python3 rebuild the stock image happens
# to ship.  The stock image also ships no pip.
PYTHON3_NVR="${CINDERX_PYTHON3_NVR:-3.11.6-34.oe2403sp3}"
dnf install -y -q "python3-${PYTHON3_NVR}" python3-pip
# Whole-string compare: a substring match would accept an NVR suffix rebuild.
test "$(rpm -q --queryformat '%{NAME}-%{VERSION}-%{RELEASE}' python3)" = "python3-${PYTHON3_NVR}"
python3 -c "import sys; assert sys.version_info[:3] == (3, 11, 6), sys.version"

# Globbing, not find(1): the stock image is minimal and has no findutils.
# Scoped to the cp311 tag: the release wheelhouse also carries the cp314
# fat wheel, and a bare glob would sort onto it.
wheels=(/wheels/cinderx-*-cp311-*.whl)
wheel="${wheels[-1]}"
test -f "$wheel"
echo "[cp311-smoke] wheel: ${wheel}"

# Hash-checking mode against the driver-written requirements file, so the
# smoke also proves the sidecar hash matches what actually installs.
reqs=(/wheels/cinderx-*-cp311-*.requirements.txt)
python3 -m pip install --no-index --no-deps --require-hashes -r "${reqs[-1]}"

# Layer 1: the native module loads at all on the stock image (this is where a
# missing GLIBCXX version would surface) and reports the expected provenance.
python3 - <<'PY'
import sys
import _cinderx
import cinderx

print("[cp311-smoke] python", sys.version.split()[0])
print("[cp311-smoke] _cinderx at", _cinderx.__file__)
assert "site-packages" in _cinderx.__file__, _cinderx.__file__
PY

# Layer 2: the full 3.11 unit suite (interpreter take-over, JIT-disabled
# gate, observe mode) against the installed wheel.
cd /smoke
python3 -m unittest -v \
    test_cinderx.test_kunpeng.test_interpreter_311 \
    test_cinderx.test_kunpeng.test_jit_unsupported_311 \
    test_cinderx.test_kunpeng.test_observe_311

echo "[cp311-smoke] PASS"
