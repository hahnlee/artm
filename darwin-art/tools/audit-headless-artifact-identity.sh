#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact="$root/_build/runtime-link-probe/libdarwin_art_runtime.dylib"
log_dir="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-headless-identity.XXXXXX")"
trap 'rm -rf -- "$log_dir"' EXIT

run_audit() {
  local log="$1"
  (cd "$root" && cargo run -q -p art-bootstrap -- audit-runtime-link) >"$log" 2>&1
}

run_audit "$log_dir/first.log"
[[ -f "$artifact" ]] || {
  echo "headless-artifact-identity: missing artifact after first audit: $artifact" >&2
  exit 1
}
first_sha="$(shasum -a 256 "$artifact" | awk '{print $1}')"

run_audit "$log_dir/second.log"
second_sha="$(shasum -a 256 "$artifact" | awk '{print $1}')"
[[ "$first_sha" == "$second_sha" ]] || {
  echo "headless-artifact-identity: hash changed between identical audits" >&2
  echo "first=$first_sha second=$second_sha" >&2
  exit 1
}

grep -a -F 'audit-runtime-link:' "$log_dir/second.log" >/dev/null || {
  echo "headless-artifact-identity: runtime-link audit did not complete" >&2
  exit 1
}
printf 'headless-artifact-identity: PASS sha256=%s artifact=%s\n' "$second_sha" "$artifact"
