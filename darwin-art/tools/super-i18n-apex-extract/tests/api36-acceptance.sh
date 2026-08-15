#!/bin/bash
set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "$0")" && pwd)"
tool_root="$(cd "$script_dir/.." && pwd)"
project_root="$(cd "$tool_root/../.." && pwd)"
image="${1:-$HOME/Library/Android/sdk/system-images/android-36/google_apis_playstore/arm64-v8a/system.img}"
expected_apex_sha=d102f218f387f7e1740df71bbb155b81adb47c95062e0fb6d92098c6e64fcd5c
expected_jar_sha=f649d9357d00e94dc7820cc41bea24125b8410e00c3e33ee9e7c0dba0ad9047c

[[ -f "$image" ]] || { echo "missing API 36 image: $image" >&2; exit 3; }
temp_root="$(mktemp -d "${TMPDIR:-/tmp}/super-i18n-apex-test.XXXXXX")"
cleanup() {
  [[ -n "$temp_root" && "$temp_root" == "${TMPDIR:-/tmp}"/super-i18n-apex-test.* ]] &&
    rm -rf "$temp_root"
}
trap cleanup EXIT

cargo run --quiet --release --manifest-path "$tool_root/Cargo.toml" -- \
  "$image" "$temp_root/com.android.i18n.apex" >"$temp_root/report"
grep -Fx "target.path=/system/apex/com.android.i18n.apex target.bytes=37642240 target.sha256=$expected_apex_sha" \
  "$temp_root/report" >/dev/null
[[ "$(shasum -a 256 "$temp_root/com.android.i18n.apex" | awk '{print $1}')" == "$expected_apex_sha" ]]

cargo run --quiet --manifest-path "$project_root/tools/apex-ext2-extract/Cargo.toml" -- \
  "$temp_root/com.android.i18n.apex" "$temp_root/core-icu4j.jar" >/dev/null
[[ "$(shasum -a 256 "$temp_root/core-icu4j.jar" | awk '{print $1}')" == "$expected_jar_sha" ]]

dexdump="${ICU_DEXDUMP:-$HOME/Library/Android/sdk/build-tools/36.0.0/dexdump}"
[[ -x "$dexdump" ]] || { echo "missing API 36 dexdump: $dexdump" >&2; exit 3; }
unzip -p "$temp_root/core-icu4j.jar" classes.dex >"$temp_root/classes.dex"
"$dexdump" -d "$temp_root/classes.dex" >"$temp_root/dexdump"
grep -A5 "name          : 'ICU_BASE_NAME'" "$temp_root/dexdump" |
  grep -F 'value         : "android/icu/impl/data/icudata"' >/dev/null
grep -A5 "name          : 'ICU_DATA_VERSION_PATH'" "$temp_root/dexdump" |
  grep -F 'value         : "76b"' >/dev/null

echo "super-i18n-apex API36: PASS apex=$expected_apex_sha core-icu4j=$expected_jar_sha VersionInfo=76b"
