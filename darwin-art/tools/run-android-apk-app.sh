#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-}"
seconds="${2:-30}"
[[ -n "$apk" ]] || {
  echo "usage: $0 APK [VISIBLE_SECONDS]" >&2
  exit 64
}
apk="$(cd "$(dirname "$apk")" && pwd)/$(basename "$apk")"
[[ -f "$apk" ]] || {
  echo "APK does not exist: $apk" >&2
  exit 66
}
[[ "$seconds" =~ ^([0-9]+)(\.[0-9]+)?$ ]] || {
  echo "VISIBLE_SECONDS must be a non-negative number" >&2
  exit 64
}

metadata="$(cargo run -q \
  --manifest-path "$root/tools/android-apk-app-runtime/Cargo.toml" -- "$apk")"
package="$(sed -n 's/^apk-app-runtime: package=\([^ ]*\) activity=.*/\1/p' <<<"$metadata")"
activity="$(sed -n 's/^apk-app-runtime: .* activity=\([^ ]*\) descriptor=.*/\1/p' <<<"$metadata")"
descriptor="$(sed -n 's/^apk-app-runtime: .* descriptor=\([^ ]*\) dex=.*/\1/p' <<<"$metadata")"
[[ -n "$package" && -n "$activity" && -n "$descriptor" ]] || {
  echo "could not decode inspected APK metadata" >&2
  exit 65
}

host="$root/target/debug/darwin-art-host"
runtime="$root/_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
core_oj="$root/_prebuilt/android-16/bootclasspath/core-oj.jar"
core_libart="$root/_prebuilt/android-16/bootclasspath/core-libart.jar"
framework="$root/_prebuilt/android-16/bootclasspath/framework.jar"
core_icu="$root/_build/bootclasspath/core-icu4j-api36.jar"
support_dex="$root/_build/dex-probe/dex/classes.dex"
fonts_xml="$root/probes/button/fonts.xml"
roboto="$root/_aosp/external/skia/resources/fonts/Roboto-Regular.ttf"
for input in "$host" "$runtime" "$core_oj" "$core_libart" "$framework" "$core_icu" "$support_dex" "$fonts_xml" "$roboto"; do
  [[ -f "$input" ]] || {
    echo "runtime input is missing: $input" >&2
    echo "run the bootstrap/graphics build gates first" >&2
    exit 69
  }
done

icu_runtime="$root/_build/icu-runtime-adapters/runtime"
export ANDROID_I18N_ROOT="$icu_runtime/i18n"
export ANDROID_DATA="$icu_runtime/data"
export ANDROID_TZDATA_ROOT="$icu_runtime/tzdata"
export DARWIN_ART_APK_APP_PACKAGE="$package"
export DARWIN_ART_APK_APP_ACTIVITY="$activity"
export DARWIN_ART_APK_APP_DESCRIPTOR="$descriptor"
export DARWIN_ART_APK_APP_SUPPORT_DEX="$support_dex"
export DARWIN_ART_TEST_FONTS_XML="$fonts_xml"
export DARWIN_ART_TEST_FONT="$roboto"

echo "$metadata"
exec "$host" --window-seconds "$seconds" \
  "$runtime" "$core_oj" "$core_libart" "$framework" "$core_icu" "$apk"
