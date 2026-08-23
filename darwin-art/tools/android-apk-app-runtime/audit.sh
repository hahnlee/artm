#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
module="$root/tools/android-apk-app-runtime"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
aapt2="$sdk/build-tools/35.0.0/aapt2"
d8="$sdk/build-tools/35.0.0/d8"
android_jar="$sdk/platforms/android-36/android.jar"
ndk_revision="28.2.13676358"
ndk="$sdk/ndk/$ndk_revision"
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64"
android_clang="$toolchain/bin/aarch64-linux-android35-clang"
build="$root/_build/android-apk-app-runtime"
classes="$build/classes"
dex="$build/dex"
apk="$build/simple-no-native.apk"
jni_apk="$build/simple-jni.apk"

[[ -x "$aapt2" && -x "$d8" && -x "$android_clang" && -f "$android_jar" ]] || {
  echo "Android SDK 35 build tools and platform 36 are required" >&2
  exit 1
}

cargo fmt --manifest-path "$module/Cargo.toml" -- --check
cargo clippy --manifest-path "$module/Cargo.toml" --all-targets -- -D warnings
cargo test --manifest-path "$module/Cargo.toml"

if [[ ! -f "$root/_build/dex-probe/dex/classes.dex" ]]; then
  cargo run -q -p art-bootstrap -- build-dex
fi

if [[ -d "$build" ]]; then
  chmod -R u+w "$build"
fi
rm -rf "$build"
mkdir -p "$classes" "$dex" "$build/jni/lib/arm64-v8a"
javac --release 8 -encoding UTF-8 \
  -classpath "$android_jar:$root/_build/dex-probe/classes" \
  -d "$classes" \
  "$module/fixture/FontBootstrap.java" \
  "$module/fixture/MainActivity.java" \
  "$module/fixture/DarwinServiceBridge.java"

app_classes=("$classes"/dev/darwinart/simple/*.class)
"$d8" --lib "$android_jar" --output "$dex" "${app_classes[@]}"

resource_zip="$build/resources.zip"
"$aapt2" compile --dir "$module/fixture/res" -o "$resource_zip"

"$aapt2" link \
  -I "$android_jar" \
  --auto-add-overlay \
  -R "$resource_zip" \
  --manifest "$module/fixture/AndroidManifest.xml" \
  --min-sdk-version 35 \
  --target-sdk-version 35 \
  -o "$apk"
(cd "$dex" && zip -q -j "$apk" classes.dex)

"$android_clang" -std=c17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror \
  -shared -nostdlib -fuse-ld=lld -Wl,--build-id=none -Wl,--hash-style=sysv \
  -Wl,-z,now -Wl,-z,norelro -Wl,-soname,libdarwin-art-simple-jni.so \
  "$module/fixture/native_app.c" -o "$build/jni/lib/arm64-v8a/libdarwin-art-simple-jni.so"
cp "$apk" "$jni_apk"
(cd "$build/jni" && zip -q -r "$jni_apk" lib)

entries="$(unzip -Z1 "$apk")"
[[ "$(grep -c '^classes\.dex$' <<<"$entries")" == 1 ]]
! grep -Eq '(^|/)classes[2-9][0-9]*\.dex$|\.so$' <<<"$entries"
dex_summary="$($root/_build/dex-probe/dex-probe "$dex/classes.dex")"
expected_dex='AOSP DEX: verified=yes version=35 classes=2 methods=15 class[0]=Ldev/darwinart/simple/FontBootstrap; class[1]=Ldev/darwinart/simple/MainActivity;'
[[ "$dex_summary" == "$expected_dex" ]]

expected='apk-app-runtime: package=dev.darwinart.simple activity=dev.darwinart.simple.MainActivity descriptor=Ldev/darwinart/simple/MainActivity; label=Darwin ART APK icon=none dex=primary native=0'
actual="$(cargo run -q --manifest-path "$module/Cargo.toml" -- "$apk")"
[[ "$actual" == "$expected" ]] || {
  printf 'unexpected inspector output:\n%s\n' "$actual" >&2
  exit 1
}
"$aapt2" dump badging "$apk" | grep -F "launchable-activity: name='dev.darwinart.simple.MainActivity'" >/dev/null

jni_entries="$(unzip -Z1 "$jni_apk")"
[[ "$(grep -c '^classes\.dex$' <<<"$jni_entries")" == 1 ]]
grep -Fx 'lib/arm64-v8a/libdarwin-art-simple-jni.so' <<<"$jni_entries" >/dev/null
file "$build/jni/lib/arm64-v8a/libdarwin-art-simple-jni.so" |
  grep -F 'ELF 64-bit LSB shared object, ARM aarch64' >/dev/null
llvm_readelf="$toolchain/bin/llvm-readelf"
"$llvm_readelf" -d "$build/jni/lib/arm64-v8a/libdarwin-art-simple-jni.so" |
  grep -F '(SONAME)' >/dev/null

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
