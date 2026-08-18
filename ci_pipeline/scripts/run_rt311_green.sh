#!/bin/bash
# Build runtime_tests for CPython 3.11 and hold the green baseline.
#
# Modes:
#   run_rt311_green.sh <build_dir>            green-family gate (default)
#   run_rt311_green.sh <build_dir> --census   full census: must run to
#                                             completion (no crash), green
#                                             families must stay green
#
# The green-family manifest is ci_pipeline/jit311/data/rt311_green_families.txt;
# HIRBuildTest is green under shadow init. Capability skips live in
# HIRBuildDeferredTest + rt311_allowed_skips.txt. Remaining families are
# the known-fail set owned by later MRs.
set -euo pipefail
# Every sort/comm/diff over the committed manifests runs in C collation:
# the committed files are byte-ordered, and a UTF-8 runner locale would
# order the live lists differently and fake an identity drift.
export LC_ALL=C
# Inherited GTEST_*/TESTBRIDGE_* could filter or shard what executes; the
# gate owns its execution surface (the PASSED==manifest check would catch
# the shrink, but a clean environment fails faster and clearer).
while IFS='=' read -r name _; do
  case "$name" in GTEST_*|TESTBRIDGE_*) unset "$name" ;; esac
done < <(env)
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MANIFEST="$REPO_ROOT/ci_pipeline/jit311/data/rt311_green_families.txt"
REGISTERED="$REPO_ROOT/ci_pipeline/jit311/data/rt311_registered_tests.txt"

registered_drift() {
  # $1: live registered-test list (sorted Suite.Test lines).  Any drift
  # against the committed manifest -- disappearance, rename, addition --
  # must be a deliberate manifest edit, never an accident: a deleted green
  # case would otherwise stay green forever.
  if ! diff <(grep -Ev '^[[:space:]]*(#|$)' "$REGISTERED") "$1"; then
    echo "registered-test identity drifted from the committed manifest;"
    echo "edit ci_pipeline/jit311/data/rt311_registered_tests.txt deliberately"
    return 1
  fi
}

no_skips_allowed() {
  # $1: gtest log.  A skip is not a pass: a green case downgraded to
  # GTEST_SKIP keeps the exit code and the ran-count, and a known failure
  # downgraded to skip would read as "fixed".  Zero skips, always.
  if grep -qE '^\[  SKIPPED \]' "$1"; then
    echo "gtest reported skipped tests; a skip is neither a pass nor a fix:"
    grep -E '^\[  SKIPPED \]' "$1"
    return 1
  fi
}

unexpected_skips() {
  # $1: gtest log.  Capability-gated 3.11 skips (shadow-only, generators,
  # slots, later MRs) live in rt311_allowed_skips.txt.  Any other skip is
  # red: it is neither a pass nor a known failure.
  ALLOWED="$REPO_ROOT/ci_pipeline/jit311/data/rt311_allowed_skips.txt"
  LIVE="$BUILD_DIR-census-skipped.txt"
  { grep -E '^\[  SKIPPED \]' "$1" || true; } \
    | sed 's/^\[  SKIPPED \] //; s/ ([0-9]* ms)$//' \
    | { grep -E '^[A-Za-z_][A-Za-z0-9_]*\.' || true; } \
    | sort -u > "$LIVE"
  EXTRA=$(comm -23 "$LIVE" <(grep -Ev '^[[:space:]]*(#|$)' "$ALLOWED" | sort -u))
  if [ -n "$EXTRA" ]; then
    echo "census: skips outside the allowed-skip manifest:"
    echo "$EXTRA"
    return 1
  fi
}

green_log_verdict() {
  # $1: green-gate log; $2: expected green population.  The PASSED count
  # must equal the expected population exactly -- ran-count alone would
  # accept skipped members -- and no test may skip.
  no_skips_allowed "$1" || return 1
  PASSED=$({ grep -E '^\[  PASSED  \] [0-9]+ tests?\.' "$1" || true; } \
    | sed -E 's/^\[  PASSED  \] ([0-9]+) tests?\./\1/' | tail -1)
  if [ "$PASSED" != "$2" ]; then
    echo "green gate passed $PASSED tests but the manifest expects $2"
    return 1
  fi
}

baseline_growth() {
  # $1: baseline as of the merge base; $2: current baseline.  Entries may
  # leave the baseline (fixes) but never enter it in the same change that
  # introduces the failure -- growth requires its own reviewed change.
  ADDED=$(comm -13 <(grep -Ev '^[[:space:]]*(#|$)' "$1" | sort -u) \
                   <(grep -Ev '^[[:space:]]*(#|$)' "$2" | sort -u))
  if [ -n "$ADDED" ]; then
    echo "known-failure baseline grew relative to the protected base:"
    echo "$ADDED"
    echo "a new failure cannot be washed green by extending the baseline"
    return 1
  fi
}

if [ "${1:-}" = "--verify-registered" ]; then
  # Self-test entry: compare a live list against the manifest without a
  # build, through the same function the gate uses.
  registered_drift "${2:?usage: --verify-registered <live-list-file>}"
  exit $?
fi
if [ "${1:-}" = "--verify-baseline-growth" ]; then
  baseline_growth "${2:?old baseline}" "${3:?new baseline}"
  exit $?
fi
if [ "${1:-}" = "--verify-skip-growth" ]; then
  baseline_growth "${2:?old skip allowlist}" "${3:?new skip allowlist}"
  exit $?
fi
if [ "${1:-}" = "--verify-green-log" ]; then
  green_log_verdict "${2:?log}" "${3:?expected count}"
  exit $?
fi

BUILD_DIR=${1:?usage: run_rt311_green.sh <build_dir> [--census]}
MODE=${2:-}

FLAGS=$(python3.11 -c '
import sys
sys.path.insert(0, sys.argv[1])
from cmake_options import cmake_feature_options
opts = cmake_feature_options(py_version="3.11")
print(" ".join(f"-D{k}={v}" for k, v in sorted(opts.items())))
' "$REPO_ROOT/ci_pipeline")
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_RUNTIME_TESTS=ON $FLAGS > "$BUILD_DIR-configure.log" 2>&1
make -C "$BUILD_DIR" -j"$(nproc)" runtime_tests > "$BUILD_DIR-build.log" 2>&1
BIN=$(find "$BUILD_DIR" -name runtime_tests -type f | head -1)

# Pin the registered-test identity before anything runs.
"$BIN" --gtest_list_tests 2>/dev/null \
  | awk '/^[A-Za-z_][A-Za-z0-9_]*\./ { suite = $1 }
         /^  [A-Za-z_]/ { print suite $1 }' \
  | sort -u > "$BUILD_DIR-registered.txt"
registered_drift "$BUILD_DIR-registered.txt"

FILTER=$(awk '{printf "%s.*:", $1}' "$MANIFEST")
if [ "$MODE" = "--census" ]; then
  set +e
  (cd "$REPO_ROOT/cinderx" && "$BIN") > "$BUILD_DIR-census.log" 2>&1
  CENSUS_CODE=$?
  set -e
  # Only clean-pass (0) or plain test failures (1) are acceptable census
  # outcomes; signals and aborts are red regardless of the log contents.
  if [ "$CENSUS_CODE" != 0 ] && [ "$CENSUS_CODE" != 1 ]; then
    echo "census exited $CENSUS_CODE (abnormal termination)"; exit 1
  fi
  # Completion, not success: the census documents the known-fail set.  A
  # crash leaves no final tally line and fails here.
  grep -qE '^\[==========\] .* ran\.' "$BUILD_DIR-census.log" \
    || { echo "census did not run to completion (crash?)"; exit 1; }
  grep -E '^\[==========\] .* ran\.' "$BUILD_DIR-census.log"
  # Capability-gated 3.11 skips are pinned in rt311_allowed_skips.txt;
  # anything else is neither a pass nor a known failure.
  unexpected_skips "$BUILD_DIR-census.log"
  # Skip allowlist may only shrink silently, never grow: adding GTEST_SKIP
  # and its allowlist row in the same change would keep census green.
  SKIP_ALLOWLIST="$REPO_ROOT/ci_pipeline/jit311/data/rt311_allowed_skips.txt"
  SKIP_BOOTSTRAP_COUNT=130
  SKIP_BOOTSTRAP_SHA256=80aae121d290fc73a5bb73c05df8411c8b83368b5377ab977009e36330d32001
  if [ -z "${RT311_BASELINE_BASE:-}" ]; then
    echo "census: RT311_BASELINE_BASE must be set to the merge-base SHA"
    echo "(the skip-allowlist self-extension guard refuses to run open)"
    exit 1
  fi
  if ! git -C "$REPO_ROOT" rev-parse --verify --quiet \
       "$RT311_BASELINE_BASE^{commit}" > /dev/null; then
    echo "census: protected base $RT311_BASELINE_BASE does not resolve"
    exit 1
  fi
  BASE_SKIPS="$BUILD_DIR-skips-at-base.txt"
  if git -C "$REPO_ROOT" show \
       "$RT311_BASELINE_BASE:ci_pipeline/jit311/data/rt311_allowed_skips.txt" \
       > "$BASE_SKIPS" 2>/dev/null; then
    baseline_growth "$BASE_SKIPS" "$SKIP_ALLOWLIST"
    echo "census: skip-allowlist growth guard held against $RT311_BASELINE_BASE"
  else
    SKIP_NORMALIZED="$BUILD_DIR-skips-normalized.txt"
    grep -Ev '^[[:space:]]*(#|$)' "$SKIP_ALLOWLIST" > "$SKIP_NORMALIZED"
    SKIP_COUNT=$(wc -l < "$SKIP_NORMALIZED" | tr -d ' ')
    SKIP_SHA=$(sha256sum "$SKIP_NORMALIZED" | awk '{print $1}')
    if [ "$SKIP_COUNT" != "$SKIP_BOOTSTRAP_COUNT" ] \
       || [ "$SKIP_SHA" != "$SKIP_BOOTSTRAP_SHA256" ]; then
      echo "census: skip allowlist absent at $RT311_BASELINE_BASE and the"
      echo "committed list does not match the audited bootstrap pin"
      echo "($SKIP_COUNT entries, sha256 $SKIP_SHA); skip-allowlist"
      echo "changes require their own reviewed change"
      exit 1
    fi
    echo "census: bootstrap skip allowlist matches the audited pin" \
      "($SKIP_BOOTSTRAP_COUNT entries)"
  fi
  # The known-failure baseline may only shrink silently, never grow: a
  # failure outside the committed manifest is a regression in a non-green
  # family and turns the census red.
  BASELINE="$REPO_ROOT/ci_pipeline/jit311/data/rt311_known_failures.txt"
  # Real gtest entries are Suite.Test; the epilogue line "N tests, listed
  # below:" starts with a digit and must never enter the baseline (fixing
  # one failure would change the count and read as a new failure).  The
  # guards keep a fully green census alive under pipefail.
  { grep -E '^\[  FAILED  \]' "$BUILD_DIR-census.log" || true; } \
    | sed 's/^\[  FAILED  \] //; s/ ([0-9]* ms)$//' \
    | { grep -E '^[A-Za-z_][A-Za-z0-9_]*\.' || true; } \
    | sort -u > "$BUILD_DIR-census-failed.txt"
  if [ ! -f "$BASELINE" ]; then
    echo "census baseline missing: $BASELINE"; exit 1
  fi
  NEW_FAILURES=$(comm -23 "$BUILD_DIR-census-failed.txt" "$BASELINE")
  if [ -n "$NEW_FAILURES" ]; then
    echo "census: failures outside the known-fail baseline:"
    echo "$NEW_FAILURES"
    exit 1
  fi
  FIXED=$(comm -13 "$BUILD_DIR-census-failed.txt" "$BASELINE" | wc -l)
  [ "$FIXED" -gt 0 ] && echo "census: $FIXED baseline entries now pass;" \
    "shrink the manifest deliberately"
  # Baseline self-extension guard, on the production path and FAIL-CLOSED:
  # "add the failure and its baseline entry in one change" must not wash
  # green.  The census requires the protected base (RT311_BASELINE_BASE,
  # the merge-base SHA); an unset variable or an unresolvable ref is red,
  # never a skip.  While the baseline file does not yet exist at the
  # protected base (the bootstrap window), the committed baseline must
  # byte-match the audited pin below -- extending it moves the digest and
  # turns red; once the file lands at the base, the git comparison takes
  # over and the pin becomes inert.
  BOOTSTRAP_COUNT=1139
  BOOTSTRAP_SHA256=04ce056c841c3a2bc84a2ac8f9be9bedfb46ddf93c4ddc612e66fbf965a9f10a
  if [ -z "${RT311_BASELINE_BASE:-}" ]; then
    echo "census: RT311_BASELINE_BASE must be set to the merge-base SHA"
    echo "(the baseline self-extension guard refuses to run open)"
    exit 1
  fi
  if ! git -C "$REPO_ROOT" rev-parse --verify --quiet \
       "$RT311_BASELINE_BASE^{commit}" > /dev/null; then
    echo "census: protected base $RT311_BASELINE_BASE does not resolve"
    exit 1
  fi
  BASE_BASELINE="$BUILD_DIR-baseline-at-base.txt"
  if git -C "$REPO_ROOT" show \
       "$RT311_BASELINE_BASE:ci_pipeline/jit311/data/rt311_known_failures.txt" \
       > "$BASE_BASELINE" 2>/dev/null; then
    baseline_growth "$BASE_BASELINE" "$BASELINE"
    echo "census: baseline growth guard held against $RT311_BASELINE_BASE"
  else
    NORMALIZED="$BUILD_DIR-baseline-normalized.txt"
    grep -Ev '^[[:space:]]*(#|$)' "$BASELINE" > "$NORMALIZED"
    LIVE_COUNT=$(wc -l < "$NORMALIZED" | tr -d ' ')
    LIVE_SHA=$(sha256sum "$NORMALIZED" | awk '{print $1}')
    if [ "$LIVE_COUNT" != "$BOOTSTRAP_COUNT" ] \
       || [ "$LIVE_SHA" != "$BOOTSTRAP_SHA256" ]; then
      echo "census: baseline absent at $RT311_BASELINE_BASE and the"
      echo "committed baseline does not match the audited bootstrap pin"
      echo "($LIVE_COUNT entries, sha256 $LIVE_SHA); baseline changes"
      echo "require their own reviewed change"
      exit 1
    fi
    echo "census: bootstrap baseline matches the audited pin" \
      "($BOOTSTRAP_COUNT entries)"
  fi
fi
# The filtered run must execute exactly the manifest's green population: a
# family name typo in the filter (0 matches, exit 0) or a vanished family
# would otherwise stay silently green.
GREEN_EXPECTED=$(awk -F. 'NR == FNR { fam[$1] = 1; next } ($1 in fam)' \
  "$MANIFEST" "$BUILD_DIR-registered.txt" | wc -l | tr -d ' ')
set +e
(cd "$REPO_ROOT/cinderx" && "$BIN" --gtest_filter="${FILTER%:}") \
  | tee "$BUILD_DIR-green.log"
GREEN_CODE=${PIPESTATUS[0]}
set -e
[ "$GREEN_CODE" = 0 ] || exit "$GREEN_CODE"
GREEN_RAN=$({ grep -E '^\[==========\] [0-9]+ tests? from' "$BUILD_DIR-green.log" || true; } \
  | sed -E 's/^\[==========\] ([0-9]+) tests? from.*/\1/' | tail -1)
if [ "$GREEN_RAN" != "$GREEN_EXPECTED" ]; then
  echo "green gate ran $GREEN_RAN tests but the registered manifest holds"
  echo "$GREEN_EXPECTED green-family tests; the green population drifted"
  exit 1
fi
green_log_verdict "$BUILD_DIR-green.log" "$GREEN_EXPECTED"
