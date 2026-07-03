#!/usr/bin/env bash
# Sync docs/cinderx-311-adaptation-plan.md to the GitCode wiki mirror page.
# Intended to run in CI after merges to dev that touch the doc, or manually.
# Requires push credentials for the wiki repo (same as normal gitcode push).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${REPO_ROOT}/docs/cinderx-311-adaptation-plan.md"
WIKI_URL="${WIKI_URL:-https://gitcode.com/qq_16646553/cinderx_oe.wiki.git}"
PAGE_NAME="${PAGE_NAME:-CinderX适配CPython-3.11技术设计书.md}"
BANNER='> ⚠️ 本页为只读镜像，源文件是代码仓 `docs/cinderx-311-adaptation-plan.md`，以仓库版本为准。修改请走代码仓 MR，请勿直接编辑本页。'

[[ -f "${SRC}" ]] || { echo "source doc not found: ${SRC}" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
git clone --depth 1 -q "${WIKI_URL}" "${TMP}/wiki"

{ printf '%s\n\n' "${BANNER}"; cat "${SRC}"; } > "${TMP}/wiki/${PAGE_NAME}"

cd "${TMP}/wiki"
if git diff --quiet; then
  echo "wiki already up to date"
  exit 0
fi
git add -A
git commit -q -m "docs: sync 3.11 技术设计书 from repo ($(date +%Y-%m-%d))"
git push -q origin main
echo "wiki synced: ${PAGE_NAME}"
