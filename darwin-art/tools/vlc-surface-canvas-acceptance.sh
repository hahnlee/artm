#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-}"
seconds="${2:-8}"
[[ -f "$apk" ]] || {
  echo "usage: $0 VLC_APK [SECONDS]" >&2
  exit 64
}

output="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-vlc-canvas.XXXXXX")"
log="$output/vlc.log"
if ! DARWIN_ART_DEBUG_ANATIVEWINDOW=1 \
    bash "$root/tools/run-android-apk-app.sh" "$apk" "$seconds" \
    >"$log" 2>&1; then
  echo "VLC launch failed; log preserved at $log" >&2
  exit 1
fi

if rg -a -q 'UnsatisfiedLinkError.*nativeLockCanvas|Could not lock surface|Fatal signal|runtime abort' "$log"; then
  echo "VLC software Canvas failed; log preserved at $log" >&2
  exit 1
fi
locks="$(rg -a -c 'ART Android ANativeWindow: lock ' "$log" || true)"
posts="$(rg -a -c 'ART Android ANativeWindow: post generation=' "$log" || true)"
if [[ "${locks:-0}" -lt 1 || "${posts:-0}" -lt 1 ]]; then
  echo "VLC did not lock and post a software Canvas; log preserved at $log" >&2
  exit 1
fi

echo "vlc-surface-canvas: PASS locks=$locks posts=$posts log=$log"
