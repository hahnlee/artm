#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
output="$root/_build/android-window-menu-acceptance"
calculator="$root/_build/aosp-apks/ExactCalculator-api28.apk"
chrome="$(find "$root/_build/installed-apps/org.chromium.chrome" -name base.apk -type f 2>/dev/null | head -1)"

[[ -f "$calculator" ]] || {
  echo "missing unchanged AOSP Calculator APK: $calculator" >&2
  exit 66
}
[[ -f "$chrome" ]] || {
  echo "missing installed Chrome APK" >&2
  exit 66
}
mkdir -p "$output"

# Never validate product APKs with stale compatibility classes. This command
# is incremental, but also enforces the exact support-DEX class/method contract.
cargo run -q --manifest-path "$root/Cargo.toml" -p art-bootstrap -- build-button-dex \
  >"$output/support-dex.log" 2>&1

common_env=(
  DARWIN_ART_WINDOW_SCALE=2
  DARWIN_ART_TEST_POINTER_HOLD_MS=50
  DARWIN_ART_DEBUG_INPUT_LATENCY=1
  DARWIN_ART_DEBUG_POINTER=1
  DARWIN_ART_DEBUG_WINDOW_LAYERS=1
  DARWIN_ART_DEBUG_VIEW_TEXT=1
)

calculator_log="$output/calculator.log"
env "${common_env[@]}" \
  DARWIN_ART_TEST_POINTER_SEQUENCE='0,0,0;340,45,2500;340,45,300;250,35,300' \
  "$root/tools/run-android-apk-app.sh" "$calculator" 8 \
  >"$calculator_log" 2>&1

grep -a -F 'view=android.widget.ActionMenuPresenter$OverflowMenuButton' \
  "$calculator_log" >/dev/null
grep -a -F 'window frame request=392x192 layout=392x192 output=392x192 at=320,8 type=1002' \
  "$calculator_log" >/dev/null
grep -a -F 'input window index=1 type=1002' "$calculator_log" | \
  grep -a -F 'bounds=320,8-712,200' >/dev/null
grep -a -E 'View text .* text=History$' "$calculator_log" >/dev/null
grep -a -F 'window remove argc=1 session=true' "$calculator_log" >/dev/null

chrome_log="$output/chrome.log"
env "${common_env[@]}" \
  DARWIN_ART_TEST_POINTER_SEQUENCE='0,0,0;315,610,6000;180,100,500' \
  "$root/tools/run-android-apk-app.sh" "$chrome" 13 \
  >"$chrome_log" 2>&1

grep -a -F 'view=org.chromium.chrome.browser.ui.bottombar.BottomBarAppMenu' \
  "$chrome_log" >/dev/null
grep -a -F 'window frame request=560x1233 layout=560x1233 output=560x1233 at=80,47 type=1002' \
  "$chrome_log" >/dev/null
grep -a -F 'input window index=1 type=1002' "$chrome_log" | \
  grep -a -F 'bounds=80,47-640,1280' >/dev/null
grep -a -F 'app:id/new_tab_menu_id' "$chrome_log" >/dev/null
grep -a -F 'window remove argc=1 session=true' "$chrome_log" >/dev/null

if grep -a -E 'FATAL EXCEPTION|Fatal signal|SIG(SEGV|BUS|ABRT|TRAP)|runtime abort' \
    "$calculator_log" "$chrome_log" >/dev/null; then
  echo 'Android window menu acceptance observed a runtime crash' >&2
  exit 1
fi

echo 'android-window-menu-acceptance: PASS Calculator=History Chrome=New-tab Android-popup=ViewRoot+InputChannel+SurfaceFlinger'
echo "logs=$output"
