#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
framework="$root/_prebuilt/android-16/bootclasspath/framework.jar"
android_jar="$HOME/Library/Android/sdk/platforms/android-35/android.jar"
out="$root/_build/android16-framework-compat"
classes="$out/classes"
mkdir -p "$classes" "$out/input"

[[ -f "$framework" && -f "$android_jar" ]] || {
  echo "android16-framework-compat: framework/android.jar missing" >&2
  exit 69
}

javac --release 8 -encoding UTF-8 -d "$classes" -classpath "$android_jar" \
  "$root/tools/android-framework-compat/src/android/provider/DeviceConfig.java"
unzip -p "$framework" classes.dex > "$out/input/framework.dex"
if [[ -f "$out/framework-compat.jar" ]]; then
  mv "$out/framework-compat.jar" "$out/framework-compat.previous.jar"
fi
"$HOME/Library/Android/sdk/build-tools/35.0.1/d8" \
  --lib "$android_jar" --output "$out" \
  "$out/input/framework.dex" \
  "$classes/android/provider/DeviceConfig.class" \
  "$classes/android/provider/DeviceConfig\$Properties.class" \
  "$classes/android/provider/DeviceConfig\$OnPropertiesChangedListener.class"
mv "$out/classes.dex" "$out/framework-compat.raw.dex"
staged="$(mktemp -d "$out/staged.XXXXXX")"
unzip -q "$framework" -d "$staged"
cp "$out/framework-compat.raw.dex" "$staged/classes.dex"
(cd "$staged" && zip -q -qr "$out/framework-compat.jar" .)
echo "android16-framework-compat: PASS $out/framework-compat.jar"
