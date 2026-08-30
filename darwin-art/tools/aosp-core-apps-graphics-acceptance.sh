#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
output="$root/_build/aosp-core-apps-graphics-acceptance"
calculator="$root/_build/aosp-apks/ExactCalculator-api28.apk"
clock="$root/_build/aosp-apks/DeskClock-api29.apk"

[[ -f "$calculator" ]] || {
  echo "missing unchanged AOSP Calculator APK: $calculator" >&2
  exit 66
}
[[ -f "$clock" ]] || {
  echo "missing unchanged AOSP DeskClock APK: $clock" >&2
  exit 66
}
mkdir -p "$output"

calculator_log="$output/calculator.log"
clock_log="$output/deskclock.log"

env \
  DARWIN_ART_WINDOW_SCALE=2 \
  DARWIN_ART_TEST_POINTER_SEQUENCE='0,0,0;125,475,1200;315,575,300;205,475,300;205,575,300;0,0,600' \
  DARWIN_ART_TEST_POINTER_HOLD_MS=50 \
  DARWIN_ART_DEBUG_INPUT_LATENCY=1 \
  DARWIN_ART_DEBUG_POINTER=1 \
  DARWIN_ART_DEBUG_VIEW_TEXT=1 \
  DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS=1 \
  "$root/tools/run-android-apk-app.sh" "$calculator" 5 \
  >"$calculator_log" 2>&1

for target in 'app:id/digit_2' 'app:id/op_add' 'app:id/digit_3' 'app:id/eq'; do
  grep -a -F "$target" "$calculator_log" >/dev/null || {
    echo "Calculator did not hit $target" >&2
    exit 1
  }
done
grep -a -E 'View text id=0x7f06002c .* text=2\+3$' "$calculator_log" >/dev/null || {
  echo 'Calculator formula did not become 2+3' >&2
  exit 1
}
grep -a -E 'View text id=0x7f06006b .* text=5$' "$calculator_log" >/dev/null || {
  echo 'Calculator result did not become 5' >&2
  exit 1
}
grep -a -E 'SurfaceTransaction: update .* visible=1 .*buffer=1' "$calculator_log" >/dev/null || {
  echo 'Calculator did not publish a visible HWUI buffer transaction' >&2
  exit 1
}

env \
  DARWIN_ART_WINDOW_SCALE=2 \
  DARWIN_ART_TEST_POINTER_SEQUENCE='0,0,0;180,60,1200' \
  DARWIN_ART_TEST_POINTER_HOLD_MS=50 \
  DARWIN_ART_DEBUG_INPUT_LATENCY=1 \
  DARWIN_ART_DEBUG_POINTER=1 \
  DARWIN_ART_DEBUG_VIEW_TEXT=1 \
  DARWIN_ART_DEBUG_SURFACE_TRANSACTIONS=1 \
  "$root/tools/run-android-apk-app.sh" "$clock" 4 \
  >"$clock_log" 2>&1

grep -a -F 'com.google.android.material.tabs.TabLayout$TabView' "$clock_log" >/dev/null || {
  echo 'DeskClock did not hit the real Material tab view' >&2
  exit 1
}
grep -a -E 'View text id=0x7f0a015b .* text=00h 00m 00s$' "$clock_log" >/dev/null || {
  echo 'DeskClock did not transition to the Timer page' >&2
  exit 1
}
grep -a -E 'SurfaceTransaction: update .* visible=1 .*buffer=1' "$clock_log" >/dev/null || {
  echo 'DeskClock did not publish a visible HWUI buffer transaction' >&2
  exit 1
}

if grep -a -E 'FATAL EXCEPTION|SIG(SEGV|BUS|ABRT)|runtime abort' \
    "$calculator_log" "$clock_log" >/dev/null; then
  echo 'AOSP core-app graphics acceptance observed a runtime crash' >&2
  exit 1
fi

echo "aosp-core-apps-graphics-acceptance: PASS Calculator=2+3=5 DeskClock=Timer common-path=HWUI+SurfaceFlinger+Metal"
echo "logs=$output"
