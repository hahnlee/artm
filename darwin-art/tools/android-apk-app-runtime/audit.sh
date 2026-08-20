#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
module="$root/tools/android-apk-app-runtime"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
aapt2="$sdk/build-tools/35.0.0/aapt2"
d8="$sdk/build-tools/35.0.0/d8"
android_jar="$sdk/platforms/android-36/android.jar"
build="$root/_build/android-apk-app-runtime"
classes="$build/classes"
dex="$build/dex"
apk="$build/simple-no-native.apk"

[[ -x "$aapt2" && -x "$d8" && -f "$android_jar" ]] || {
  echo "Android SDK 35 build tools and platform 36 are required" >&2
  exit 1
}

cargo fmt --manifest-path "$module/Cargo.toml" -- --check
cargo clippy --manifest-path "$module/Cargo.toml" --all-targets -- -D warnings
cargo test --manifest-path "$module/Cargo.toml"

if [[ ! -f "$root/_build/dex-probe/dex/classes.dex" ]]; then
  cargo run -q -p art-bootstrap -- build-dex
fi

rm -rf "$build"
mkdir -p "$classes" "$dex"
javac --release 8 -encoding UTF-8 \
  -classpath "$android_jar:$root/_build/dex-probe/classes" \
  -d "$classes" \
  "$module/fixture/FontBootstrap.java" \
  "$module/fixture/MainActivity.java" \
  "$module/fixture/DarwinServiceBridge.java"

app_classes=("$classes"/dev/darwinart/simple/*.class)
"$d8" --lib "$android_jar" --output "$dex" "${app_classes[@]}"

"$aapt2" link \
  -I "$android_jar" \
  --manifest "$module/fixture/AndroidManifest.xml" \
  --min-sdk-version 35 \
  --target-sdk-version 35 \
  -o "$apk"
(cd "$dex" && zip -q -j "$apk" classes.dex)

entries="$(unzip -Z1 "$apk")"
[[ "$(grep -c '^classes\.dex$' <<<"$entries")" == 1 ]]
! grep -Eq '(^|/)classes[2-9][0-9]*\.dex$|\.so$' <<<"$entries"
dex_summary="$($root/_build/dex-probe/dex-probe "$dex/classes.dex")"
expected_dex='AOSP DEX: verified=yes version=35 classes=2 methods=15 class[0]=Ldev/darwinart/simple/FontBootstrap; class[1]=Ldev/darwinart/simple/MainActivity;'
[[ "$dex_summary" == "$expected_dex" ]]

expected='apk-app-runtime: package=dev.darwinart.simple activity=dev.darwinart.simple.MainActivity descriptor=Ldev/darwinart/simple/MainActivity; dex=primary native=0'
actual="$(cargo run -q --manifest-path "$module/Cargo.toml" -- "$apk")"
[[ "$actual" == "$expected" ]] || {
  printf 'unexpected inspector output:\n%s\n' "$actual" >&2
  exit 1
}
"$aapt2" dump badging "$apk" | grep -F "launchable-activity: name='dev.darwinart.simple.MainActivity'" >/dev/null

native_apk="$build/reject-native.apk"
cp "$apk" "$native_apk"
mkdir -p "$build/reject/lib/arm64-v8a"
: >"$build/reject/lib/arm64-v8a/libforbidden.so"
(cd "$build/reject" && zip -q "$native_apk" lib/arm64-v8a/libforbidden.so)
if cargo run -q --manifest-path "$module/Cargo.toml" -- "$native_apk" 2>"$build/reject-native.err"; then
  echo "inspector accepted an APK containing a native .so" >&2
  exit 1
fi
grep -F 'APK contains a native .so entry' "$build/reject-native.err" >/dev/null

secondary_apk="$build/reject-secondary-dex.apk"
cp "$apk" "$secondary_apk"
cp "$dex/classes.dex" "$build/classes2.dex"
(cd "$build" && zip -q "$secondary_apk" classes2.dex)
if cargo run -q --manifest-path "$module/Cargo.toml" -- "$secondary_apk" 2>"$build/reject-secondary.err"; then
  echo "inspector accepted a secondary DEX" >&2
  exit 1
fi
grep -F 'one primary classes.dex only' "$build/reject-secondary.err" >/dev/null

git -C "$root" diff --check
echo "android-apk-app-runtime: PASS APK=real-binary-manifest classes.dex=single/app-only-2 native-so=0 launcher=dev.darwinart.simple.MainActivity"
