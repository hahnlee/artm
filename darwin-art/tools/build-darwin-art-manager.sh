#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
source_root="$project_root/apps/DarwinARTManager"
app="$project_root/_build/Darwin ART Manager.app"
contents="$app/Contents"
binary="$contents/MacOS/DarwinARTManager"
shim_launcher="$contents/Resources/DarwinARTAppLauncher"
sdk="$(xcrun --sdk macosx --show-sdk-path)"

[[ "$app" == "$project_root/_build/Darwin ART Manager.app" ]] || {
  echo "refusing unexpected manager output: $app" >&2
  exit 70
}
rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Resources/DarwinART"
cp "$source_root/Info.plist" "$contents/Info.plist"
xcrun clang -arch arm64 -isysroot "$sdk" -mmacosx-version-min=14.0 \
  -fobjc-arc -fmodules -Wall -Wextra -Werror \
  -framework AppKit -framework ServiceManagement -framework UniformTypeIdentifiers \
  "$source_root"/Sources/*.m -o "$binary"
xcrun clang -arch arm64 -isysroot "$sdk" -mmacosx-version-min=14.0 \
  -fobjc-arc -fmodules -Wall -Wextra -Werror \
  -framework AppKit \
  "$source_root/AppShim/DARAppShimMain.m" -o "$shim_launcher"

cargo build --manifest-path "$project_root/Cargo.toml" -q --release \
  -p darwin-art-host \
  -p darwin-art-profile --bins \
  -p darwin-art-apk-install \
  -p darwin-art-native-artifact --bin darwin-art-native-resolve
cargo build -q --release \
  --manifest-path "$project_root/tools/android-apk-app-runtime/Cargo.toml"
cargo build -q --release \
  --manifest-path "$project_root/tools/android-apk-native-extract/Cargo.toml"
"$project_root/tools/materialize-moltenvk.sh" >/dev/null

runtime="$contents/Resources/DarwinART"
copy_file() {
  local source="$1"
  local destination="$runtime/$2"
  [[ -f "$source" ]] || {
    echo "manager runtime input missing: $source" >&2
    exit 69
  }
  mkdir -p "$(dirname "$destination")"
  cp "$source" "$destination"
}
copy_tree() {
  local source="$1"
  local destination="$runtime/$2"
  [[ -d "$source" ]] || {
    echo "manager runtime tree missing: $source" >&2
    exit 69
  }
  mkdir -p "$(dirname "$destination")"
  ditto "$source" "$destination"
}

copy_file "$project_root/target/release/darwin-art-host" target/release/darwin-art-host
for helper in darwin-artctl darwin-artd darwin-art-apk-install \
  darwin-art-native-resolve android-apk-app-runtime android-apk-native-extract; do
  copy_file "$project_root/target/release/$helper" "target/release/$helper"
done
copy_file "$project_root/tools/run-android-apk-app.sh" tools/run-android-apk-app.sh
copy_file "$project_root/config/darwin-art-host.entitlements" config/darwin-art-host.entitlements
copy_file "$project_root/_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib" \
  _build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib
copy_file "$project_root/_build/android16-core-oj-compat/core-oj-compat.jar" \
  _build/android16-core-oj-compat/core-oj-compat.jar
copy_file "$project_root/_build/android16-framework-compat/framework-compat.jar" \
  _build/android16-framework-compat/framework-compat.jar
copy_file "$project_root/_build/bootclasspath/core-icu4j-api36.jar" \
  _build/bootclasspath/core-icu4j-api36.jar
copy_file "$project_root/_build/button-dex/dex/classes.dex" _build/button-dex/dex/classes.dex
copy_tree "$project_root/_build/icu-runtime-adapters/runtime" _build/icu-runtime-adapters/runtime
copy_file "$project_root/_prebuilt/android-16/bootclasspath/core-libart.jar" \
  _prebuilt/android-16/bootclasspath/core-libart.jar
copy_file "$project_root/_prebuilt/android-16/bootclasspath/framework-location.jar" \
  _prebuilt/android-16/bootclasspath/framework-location.jar
copy_file "$project_root/_prebuilt/android-16/resources/framework-res.apk" \
  _prebuilt/android-16/resources/framework-res.apk
copy_file "$project_root/probes/button/fonts.xml" probes/button/fonts.xml
copy_file "$project_root/_aosp/external/skia/resources/fonts/Roboto-Regular.ttf" \
  _aosp/external/skia/resources/fonts/Roboto-Regular.ttf
for relative in \
  conscrypt/javalib/conscrypt.jar \
  conscrypt/lib64/libc++.so \
  conscrypt/lib64/libcrypto.so \
  conscrypt/lib64/libjavacrypto.so \
  conscrypt/lib64/libssl.so \
  bt/javalib/framework-bluetooth.jar \
  mediaprovider/javalib/framework-mediaprovider.jar \
  permission/javalib/framework-permission.jar \
  permission/javalib/framework-permission-s.jar; do
  copy_file "$project_root/_build/android16-ps16k-r07/extracted/$relative" \
    "_build/android16-ps16k-r07/extracted/$relative"
done
if [[ -f "$project_root/_build/android-unwind-provider/libdarwin_art_android_unwind.so" ]]; then
  copy_file "$project_root/_build/android-unwind-provider/libdarwin_art_android_unwind.so" \
    _build/android-unwind-provider/libdarwin_art_android_unwind.so
fi
for library in "$project_root/_build/angle-source/out/DarwinArtRelease/"*.dylib; do
  copy_file "$library" "_build/angle-source/out/DarwinArtRelease/$(basename "$library")"
done
copy_file "$project_root/_build/moltenvk/libMoltenVK.dylib" \
  _build/moltenvk/libMoltenVK.dylib
copy_file "$project_root/_build/moltenvk/LICENSE" _build/moltenvk/LICENSE

runtime_dylib="$runtime/_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
lz4_source="$(brew --prefix lz4)/lib/liblz4.1.dylib"
copy_file "$lz4_source" _build/runtime-graphics-link-probe/liblz4.1.dylib
install_name_tool -change "$lz4_source" @loader_path/liblz4.1.dylib "$runtime_dylib"

chmod +x "$runtime/tools/run-android-apk-app.sh" "$runtime/target/release/darwin-art-host" \
  "$runtime/target/release/"*
codesign --force --sign - --options runtime --timestamp=none \
  --entitlements "$project_root/config/darwin-art-host.entitlements" \
  "$runtime/target/release/darwin-art-host" >/dev/null
find "$runtime" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 |
  while IFS= read -r -d '' library; do
    codesign --force --sign - --timestamp=none "$library" >/dev/null
  done
for helper in "$runtime/target/release/"*; do
  codesign --force --sign - --timestamp=none "$helper" >/dev/null
done
codesign --force --sign - --timestamp=none "$shim_launcher" >/dev/null
codesign --force --sign - --timestamp=none "$app" >/dev/null
plutil -lint "$contents/Info.plist" >/dev/null
codesign --verify --deep --strict "$app"

echo "$app"
