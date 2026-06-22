#!/usr/bin/env bash
set -euo pipefail

ENV_FILE="${CINDERX_WEBHOOK_ENV_FILE:-/etc/cinderx-webhook.env}"

read -r -s -p "GitCode access token: " token
echo

if [ -z "$token" ]; then
  echo "empty token; aborting" >&2
  exit 1
fi

mkdir -p "$(dirname "$ENV_FILE")"
tmp_file="$(mktemp)"
if [ -f "$ENV_FILE" ]; then
  grep -v '^GITCODE_ACCESS_TOKEN=' "$ENV_FILE" > "$tmp_file"
fi
printf 'GITCODE_ACCESS_TOKEN=%s\n' "$token" >> "$tmp_file"
mv "$tmp_file" "$ENV_FILE"

chmod 600 "$ENV_FILE"
echo "token saved to $ENV_FILE"
