#!/usr/bin/env bash
set -euo pipefail

# Repeat real MotionEvent DOWN/UP pairs against the unmodified Chrome APK
# without enabling the verbose diagnostic switches used by acceptance tests.
# The host's benchmark mode emits one aggregate owner-thread latency line.
root="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-}"
event_count="${2:-100}"
delay_ms="${3:-50}"
hold_ms="${4:-18}"

[[ -n "$apk" && -f "$apk" ]] || {
  echo "usage: $0 CHROME_APK [TAP_COUNT=100] [INTERVAL_MS=50] [HOLD_MS=18]" >&2
  exit 64
}
[[ "$event_count" =~ ^[1-9][0-9]*$ && "$delay_ms" =~ ^[0-9]+$ && "$hold_ms" =~ ^[0-9]+$ ]] || {
  echo "TAP_COUNT, INTERVAL_MS, and HOLD_MS must be non-negative integers" >&2
  exit 64
}

# Avoid inheriting any per-event or frame-timing diagnostics from an
# interactive shell. DARWIN_ART_BENCHMARK itself is intentionally retained.
unset DARWIN_ART_DEBUG_INPUT_LATENCY DARWIN_ART_DEBUG_FRAME_TIMING \
  DARWIN_ART_DEBUG_SURFACE_STATS DARWIN_ART_DEBUG_POINTER

sequence="0,0,0"
for ((index = 0; index < event_count; index++)); do
  sequence+=";180,320,$delay_ms"
done

# Keep each run isolated from an existing Chrome profile. The launcher owns
# the Android package sandbox below this root; it is safe to inspect afterward
# and is deliberately left in /tmp for reproducibility.
artifact="$(mktemp -d /tmp/darwin-art-chromium-benchmark.XXXXXX)"
visible_seconds=$((event_count * (delay_ms + hold_ms) / 1000 + 8))
if (( visible_seconds < 12 )); then
  visible_seconds=12
fi

echo "benchmark: taps=$event_count interval_ms=$delay_ms hold_ms=$hold_ms visible_seconds=$visible_seconds"
echo "benchmark: artifact=$artifact"

set +e
DARWIN_ART_BENCHMARK=1 \
DARWIN_ART_APP_DATA_ROOT="$artifact/app-data" \
DARWIN_ART_TEST_POINTER_SEQUENCE="$sequence" \
DARWIN_ART_TEST_POINTER_HOLD_MS="$hold_ms" \
  "$root/tools/run-android-apk-app.sh" "$apk" "$visible_seconds" \
  >"$artifact/launch.log" 2>&1
status=$?
set -e
if (( status != 0 )); then
  tail -40 "$artifact/launch.log" >&2
  echo "benchmark: launcher failed status=$status artifact=$artifact" >&2
  exit "$status"
fi

summary="$(grep -F 'DARWIN_ART input->framework-pulse samples=' "$artifact/launch.log" | tail -1 || true)"
[[ -n "$summary" ]] || {
  tail -40 "$artifact/launch.log" >&2
  echo "benchmark: missing latency summary artifact=$artifact" >&2
  exit 70
}
sample_count="$(sed -n 's/.*samples=\([0-9][0-9]*\).*/\1/p' <<<"$summary")"
[[ "$sample_count" =~ ^[0-9]+$ && "$sample_count" -ge "$event_count" ]] || {
  echo "benchmark: expected at least $event_count samples, got ${sample_count:-none}" >&2
  exit 71
}
echo "$summary"
echo "benchmark: PASS samples=$sample_count artifact=$artifact"
