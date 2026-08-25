#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
framework="$root/_prebuilt/android-16/bootclasspath/framework.jar"
core_oj="$root/_prebuilt/android-16/bootclasspath/core-oj.jar"
android_jar="$HOME/Library/Android/sdk/platforms/android-36/android.jar"
out="$root/_build/android16-framework-compat"
classes="$out/classes"
source_root="$root/_aosp/android16-sdkextensions-compat"
lock="$root/upstream/android16-sdkextensions-compat.lock"
sdkextensions_patch="$root/patches/sdkextensions/0001-detached-framework-sdk-level.patch"
mkdir -p "$classes" "$out/input" "$source_root/java/android/os/ext"

# shellcheck disable=SC1090
source "$lock"
sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
sdkextensions_source="$source_root/$SDKEXTENSIONS_SOURCE"
if [[ ! -f "$sdkextensions_source" ]]; then
  staged_download="$(mktemp "${TMPDIR:-/tmp}/darwin-art-sdkextensions.XXXXXX")"
  /usr/bin/curl -fsSL \
    "https://android.googlesource.com/$SDKEXTENSIONS_PROJECT/+/$SDKEXTENSIONS_REVISION/$SDKEXTENSIONS_SOURCE?format=TEXT" \
    | /usr/bin/base64 -D > "$staged_download"
  [[ "$(sha256 "$staged_download")" == "$SDKEXTENSIONS_SOURCE_SHA256" ]]
  mv "$staged_download" "$sdkextensions_source"
fi
[[ "$(sha256 "$sdkextensions_source")" == "$SDKEXTENSIONS_SOURCE_SHA256" ]]
[[ "$(sha256 "$sdkextensions_patch")" == "$SDKEXTENSIONS_PATCH_SHA256" ]]
patched_root="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-sdkextensions-source.XXXXXX")"
trap 'rm -rf "$patched_root"' EXIT
mkdir -p "$patched_root/java/android/os/ext"
cp "$sdkextensions_source" "$patched_root/$SDKEXTENSIONS_SOURCE"
patch --batch --forward -p1 -d "$patched_root" < "$sdkextensions_patch" >/dev/null
patched_sdkextensions="$patched_root/$SDKEXTENSIONS_SOURCE"
[[ "$(sha256 "$patched_sdkextensions")" == "$PATCHED_SDKEXTENSIONS_SHA256" ]]

[[ -f "$framework" && -f "$core_oj" && -f "$android_jar" ]] || {
  echo "android16-framework-compat: framework/android.jar missing" >&2
  exit 69
}

javac --release 8 -encoding UTF-8 -d "$classes" -classpath "$android_jar" \
  "$root/tools/android-framework-compat/compile-stubs/android/annotation/IntDef.java" \
  "$root/tools/android-framework-compat/compile-stubs/android/annotation/NonNull.java" \
  "$root/tools/android-framework-compat/compile-stubs/android/os/SystemProperties.java" \
  "$patched_sdkextensions" \
  "$root/tools/android-framework-compat/src/android/provider/DeviceConfig.java" \
  "$root/tools/android-framework-compat/src/android/util/StatsEvent.java" \
  "$root/tools/android-framework-compat/src/android/util/StatsLog.java" \
  "$root/compat/java/android/media/MediaCommunicationManager.java" \
  "$root/compat/java/android/net/ConnectivityManager.java" \
  "$root/compat/java/android/net/Network.java" \
  "$root/compat/java/android/net/NetworkCapabilities.java" \
  "$root/compat/java/android/net/NetworkInfo.java" \
  "$root/tools/android-framework-compat/src/dev/darwinart/security/DarwinSecurityProvider.java" \
  "$root/tools/android-framework-compat/src/dev/darwinart/security/DarwinSecureRandom.java"
unzip -p "$framework" classes.dex > "$out/input/framework.dex"
if [[ -f "$out/framework-compat.jar" ]]; then
  mv "$out/framework-compat.jar" "$out/framework-compat.previous.jar"
fi
"$HOME/Library/Android/sdk/build-tools/35.0.1/d8" \
  --lib "$android_jar" --output "$out" \
  "$out/input/framework.dex" \
  "$classes/android/provider/DeviceConfig.class" \
  "$classes/android/provider/DeviceConfig\$Properties.class" \
  "$classes/android/provider/DeviceConfig\$OnPropertiesChangedListener.class" \
  "$classes/android/util/StatsEvent.class" \
  "$classes/android/util/StatsEvent\$Builder.class" \
  "$classes/android/util/StatsLog.class" \
  "$classes/android/media/MediaCommunicationManager.class" \
  "$classes/android/media/MediaCommunicationManager\$SessionCallback.class" \
  "$classes/android/net/ConnectivityManager.class" \
  "$classes/android/net/ConnectivityManager\$NetworkCallback.class" \
  "$classes/android/net/Network.class" \
  "$classes/android/net/NetworkCapabilities.class" \
  "$classes/android/net/NetworkInfo.class" \
  "$classes/dev/darwinart/security/DarwinSecurityProvider.class" \
  "$classes/dev/darwinart/security/DarwinSecureRandom.class" \
  "$classes/android/os/ext/SdkExtensions.class"
mv "$out/classes.dex" "$out/framework-compat.raw.dex"
staged="$(mktemp -d "$out/staged.XXXXXX")"
unzip -q "$framework" -d "$staged"
cp "$out/framework-compat.raw.dex" "$staged/classes.dex"
(cd "$staged" && zip -q -qr "$out/framework-compat.jar" .)
core_out="$root/_build/android16-core-oj-compat"
mkdir -p "$core_out"
core_staged="$(mktemp -d "$core_out/staged.XXXXXX")"
unzip -q "$core_oj" -d "$core_staged"
cat > "$core_staged/java/security/security.properties" <<'EOF'
security.provider.1=dev.darwinart.security.DarwinSecurityProvider
securerandom.source=file:/dev/urandom
EOF
(cd "$core_staged" && zip -q -qr "$core_out/core-oj-compat.jar" .)
echo "android16-framework-compat: PASS $out/framework-compat.jar"
