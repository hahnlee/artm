#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-}"
[[ -n "$apk" && -f "$apk" ]] || {
  echo "usage: $0 CHROME_PUBLIC_APK" >&2
  exit 64
}
apk="$(cd "$(dirname "$apk")" && pwd)/$(basename "$apk")"
"$root/tools/materialize-moltenvk.sh" >/dev/null

mkdir -p "$root/_build/chromium-tab-graphics-acceptance"
output="$(mktemp -d "$root/_build/chromium-tab-graphics-acceptance/run.XXXXXX")"
app_data="$output/app-data"
mkdir -p "$app_data"

profile_ctl="$root/target/release/darwin-artctl"
[[ -x "$profile_ctl" ]] || {
  echo "missing profile controller: $profile_ctl" >&2
  exit 69
}
profile_mount="$($profile_ctl ensure)"
central_log="${profile_mount%/mnt}/darwin-artd.log"

# Pixel evidence is emitted by the long-lived central SurfaceFlinger service,
# not the app process. Restart only the exact profile-scoped android.system
# process so this run's guarded diagnostics are inherited deterministically.
system_pid="$($profile_ctl ps | awk '$2 == "android.system" { print $1; exit }')"
if [[ -n "$system_pid" ]] && kill -0 "$system_pid" 2>/dev/null; then
  kill "$system_pid"
  for _ in $(seq 1 100); do
    kill -0 "$system_pid" 2>/dev/null || break
    sleep 0.05
  done
  kill -0 "$system_pid" 2>/dev/null && {
    echo "android.system did not stop for an isolated graphics acceptance run" >&2
    exit 1
  }
fi

start_line=$(( $(wc -l < "$central_log") + 1 ))
app_log="$output/chrome.log"
env \
  DARWIN_ART_APP_DATA_ROOT="$app_data" \
  DARWIN_ART_WINDOW_SCALE=2 \
  # Cold Chromium startup can finish the tab-switcher transition after the
  # first click's callback has returned. Leave a full display interval window
  # for the real TabGridView tree to attach before selecting its first card.
  DARWIN_ART_TEST_POINTER_SEQUENCE='0,0,0;225,610,20000;90,320,10000' \
  DARWIN_ART_TEST_POINTER_HOLD_MS=18 \
  DARWIN_ART_DEBUG_INPUT_LATENCY=1 \
  DARWIN_ART_DEBUG_POINTER=1 \
  DARWIN_ART_DEBUG_GRAPHICS_DSO=1 \
  DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS=1 \
  DARWIN_ART_DEBUG_SURFACECONTROL_PIXELS=1 \
  "$root/tools/run-android-apk-app.sh" "$apk" 34 >"$app_log" 2>&1
tail -n +"$start_line" "$central_log" >"$output/surfaceflinger.log"

grep -a -F 'org.chromium.chrome.browser.ui.android.bars_common.TabSwitcherButtonView' \
  "$app_log" >/dev/null || {
  echo 'Chrome did not hit its real tab-switcher button' >&2
  exit 1
}
grep -a -F 'org.chromium.chrome.browser.tasks.tab_management.TabGridView' \
  "$app_log" >/dev/null || {
  echo 'Chrome did not open/select a real tab-grid card' >&2
  exit 1
}
handled_count="$(grep -a -c 'InputEvent finish .* handled=1' "$app_log")"
[[ "$handled_count" -ge 4 ]] || {
  echo "Chrome tab input did not finish four Android packets: $handled_count" >&2
  exit 1
}
grep -a -F 'name=org.chromium.chrome/ChromeChildSurface' "$app_log" >/dev/null || {
  echo 'Chrome GPU service did not publish its child SurfaceControl' >&2
  exit 1
}
grep -a -F 'ART Android Vulkan: Metal provider=' "$app_log" >/dev/null || {
  echo 'Chrome did not select the packaged Vulkan-to-Metal provider' >&2
  exit 1
}
grep -a -F 'ANGLE Metal Renderer' "$app_log" >/dev/null || {
  echo 'Chrome did not select the supported GLES/ANGLE Metal renderer' >&2
  exit 1
}

unique_hashes="$(
  grep -a -o 'target pixels .* hash=[0-9a-f]*' "$output/surfaceflinger.log" |
    sed 's/.*hash=//' | sort -u | wc -l | tr -d ' '
)"
[[ "$unique_hashes" -ge 6 ]] || {
  echo "Chrome page/tab-hub/return did not produce enough composed states: $unique_hashes" >&2
  exit 1
}
grep -a -E 'source pixels .*\[[0-9]+,[0-9]+\]=([1-9][0-9]*|0,[1-9]|0,0,[1-9])' \
  "$output/surfaceflinger.log" >/dev/null || {
  echo 'Chrome SurfaceFlinger sources remained all-zero' >&2
  exit 1
}
if grep -a -E 'Fatal signal|SIG(SEGV|BUS|ABRT)|runtime abort|poison address' \
    "$app_log" >/dev/null; then
  echo 'Chrome or one of its Android services crashed during tab graphics acceptance' >&2
  exit 1
fi
if grep -a -E 'Could not find SharedImageBackingFactory|Failed to create Dawn context provider' \
    "$app_log" >/dev/null; then
  echo 'Chrome could not satisfy its thread-safe Graphite SharedImage contract' >&2
  exit 1
fi

echo "chromium-tab-graphics-acceptance: PASS actual-views=button+grid input=MotionEvent target-states=$unique_hashes GPU=GLES+ANGLE+Graphite+Dawn+MoltenVK+AHB+SurfaceFlinger+Metal"
echo "artifacts=$output"
