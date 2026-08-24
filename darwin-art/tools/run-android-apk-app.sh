#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
apk="${1:-}"
seconds="${2:-86400}"
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

app_dex="$apk"
external_dex="${apk%.apk}.dex"
if ! unzip -Z1 "$apk" | grep -Fx 'classes.dex' >/dev/null; then
  [[ -f "$external_dex" ]] || {
    echo "preoptimized APK requires its deoptimized DEX sidecar: $external_dex" >&2
    exit 69
  }
  app_dex="$external_dex"
fi
if [[ "$app_dex" == "$apk" ]]; then
  metadata="$(cargo run -q \
    --manifest-path "$root/tools/android-apk-app-runtime/Cargo.toml" -- "$apk")"
else
  metadata="$(cargo run -q \
    --manifest-path "$root/tools/android-apk-app-runtime/Cargo.toml" -- "$apk" "$app_dex")"
fi
package="$(sed -n 's/^apk-app-runtime: package=\([^ ]*\) .*/\1/p' <<<"$metadata")"
application="$(sed -n 's/^apk-app-runtime: .* application=\([^ ]*\) .*/\1/p' <<<"$metadata")"
activity="$(sed -n 's/^apk-app-runtime: .* activity=\([^ ]*\) .*/\1/p' <<<"$metadata")"
descriptor="$(sed -n 's/^apk-app-runtime: .* descriptor=\([^ ]*\) .*/\1/p' <<<"$metadata")"
version_code="$(sed -n 's/^apk-app-runtime: .* version_code=\([^ ]*\) .*/\1/p' <<<"$metadata")"
version_name="$(sed -n 's/^apk-app-runtime: .* version_name=\([^ ]*\) .*/\1/p' <<<"$metadata")"
theme="$(sed -n 's/^apk-app-runtime: .* theme=\([^ ]*\) .*/\1/p' <<<"$metadata")"
target_sdk="$(sed -n 's/^apk-app-runtime: .* target_sdk=\([^ ]*\) .*/\1/p' <<<"$metadata")"
label="$(sed -n 's/^apk-app-runtime: .* label=\(.*\) label_res=.*/\1/p' <<<"$metadata")"
label_res="$(sed -n 's/^apk-app-runtime: .* label_res=\([^ ]*\) .*/\1/p' <<<"$metadata")"
icon="$(sed -n 's/^apk-app-runtime: .* icon=\([^ ]*\) .*/\1/p' <<<"$metadata")"
[[ -n "$package" && -n "$application" && -n "$activity" && -n "$descriptor" && -n "$version_code" && -n "$version_name" && -n "$theme" && -n "$target_sdk" && -n "$label" && -n "$label_res" && -n "$icon" ]] || {
  echo "could not decode inspected APK metadata" >&2
  exit 65
}

host="$root/target/debug/darwin-art-host"
runtime="$root/_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
core_oj="$root/_prebuilt/android-16/bootclasspath/core-oj.jar"
core_libart="$root/_prebuilt/android-16/bootclasspath/core-libart.jar"
framework="$root/_prebuilt/android-16/bootclasspath/framework.jar"
if [[ -f "$root/_build/android16-framework-compat/framework-compat.jar" ]]; then
  # The detached host has no DeviceConfig service manager.  Use the merged
  # framework boot image whose no-service DeviceConfig seam preserves AOSP
  # default-valued feature flags during widget construction.
  framework="$root/_build/android16-framework-compat/framework-compat.jar"
fi
core_icu="$root/_build/bootclasspath/core-icu4j-api36.jar"
support_dex="$root/_build/button-dex/dex/classes.dex"
fonts_xml="$root/probes/button/fonts.xml"
roboto="$root/_aosp/external/skia/resources/fonts/Roboto-Regular.ttf"
framework_res="$root/_prebuilt/android-16/resources/framework-res.apk"
if [[ ! -f "$support_dex" ]]; then
  cargo run -q -p art-bootstrap -- build-button-dex >/dev/null
fi
for input in "$host" "$runtime" "$core_oj" "$core_libart" "$framework" "$core_icu" "$support_dex" "$fonts_xml" "$roboto" "$framework_res"; do
  [[ -f "$input" ]] || {
    echo "runtime input is missing: $input" >&2
    echo "run the bootstrap/graphics build gates first" >&2
    exit 69
  }
done

# The Android font bootstrap uses the guest filesystem facade. Give arbitrary
# no-native APKs the same immutable, minimal system root as the in-tree gate;
# pointing the process at the host filesystem would bypass the guest path
# policy and make results depend on the developer machine.
system_root="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-apk-system-root.XXXXXX")"
icon_file=""
cleanup_system_root() {
  rm -rf "$system_root"
  [[ -z "$icon_file" ]] || rm -f "$icon_file"
}
trap cleanup_system_root EXIT
mkdir -p "$system_root/etc" "$system_root/fonts" "$system_root/framework"
cp "$fonts_xml" "$system_root/etc/fonts.xml"
cp "$roboto" "$system_root/fonts/Roboto-Regular.ttf"
cp "$framework_res" "$system_root/framework/framework-res.apk"
chmod 0400 "$system_root/etc/fonts.xml" "$system_root/fonts/Roboto-Regular.ttf" \
  "$system_root/framework/framework-res.apk"
chmod 0500 "$system_root" "$system_root/etc" "$system_root/fonts" \
  "$system_root/framework"

icu_runtime="$root/_build/icu-runtime-adapters/runtime"
export ANDROID_I18N_ROOT="$icu_runtime/i18n"
export ANDROID_DATA="$icu_runtime/data"
export ANDROID_TZDATA_ROOT="$icu_runtime/tzdata"
export DARWIN_ART_APK_APP_PACKAGE="$package"
export DARWIN_ART_APK_APP_APPLICATION="$application"
export DARWIN_ART_APK_APP_ACTIVITY="$activity"
export DARWIN_ART_APK_APP_DESCRIPTOR="$descriptor"
export DARWIN_ART_APK_APP_VERSION_CODE="$version_code"
export DARWIN_ART_APK_APP_VERSION_NAME="$version_name"
export DARWIN_ART_APK_APP_THEME="$theme"
export DARWIN_ART_APK_APP_TARGET_SDK="$target_sdk"
export DARWIN_ART_APK_APP_LABEL="$label"
export DARWIN_ART_APK_APP_LABEL_RES="$label_res"
app_data_root="${DARWIN_ART_APP_DATA_ROOT:-$root/_build/app-data}"
app_data_dir="$app_data_root/$package"
mkdir -p "$app_data_dir"
export DARWIN_ART_APK_APP_DATA_DIR="$app_data_dir"
if [[ "$icon" != "none" ]]; then
  icon_file="$(mktemp "${TMPDIR:-/tmp}/darwin-art-apk-icon.XXXXXX")"
  unzip -p "$apk" "$icon" >"$icon_file"
  chmod 0400 "$icon_file"
  export DARWIN_ART_APK_APP_ICON="$icon_file"
else
  unset DARWIN_ART_APK_APP_ICON
fi
export DARWIN_ART_APK_APP_SUPPORT_DEX="$support_dex"
export DARWIN_ART_APK_APP_RESOURCE_APK="$apk"
export DARWIN_ART_FRAMEWORK_RES_APK="$framework_res"
export DARWIN_ART_TEST_FONTS_XML="$fonts_xml"
export DARWIN_ART_TEST_FONT="$roboto"
export DARWIN_ART_ANDROID_SYSTEM_ROOT="$system_root"
export DARWIN_ART_WINDOW_SCALE=2

echo "$metadata"
exec "$host" --window-seconds "$seconds" \
  "$runtime" "$core_oj" "$core_libart" "$framework" "$core_icu" "$app_dex"
