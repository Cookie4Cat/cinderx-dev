#!/bin/bash
# AddressSanitizer compile leg for the CPython 3.11 build (dev plan MR-02
# acceptance item 9, wired by MR-01).  Builds the full source set with ASAN
# instrumentation; running instrumented suites is a later, heavier leg.
set -euo pipefail
BUILD_DIR=${1:?usage: run_asan_build_311.sh <build_dir>}
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

FLAGS=$(python3.11 -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address" \
  $FLAGS > "$BUILD_DIR-configure.log" 2>&1
make -C "$BUILD_DIR" -j"$(nproc)" > "$BUILD_DIR-build.log" 2>&1
echo "asan build ok: $BUILD_DIR"
