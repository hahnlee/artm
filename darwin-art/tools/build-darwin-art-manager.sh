#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
source_root="$project_root/apps/DarwinARTManager"
app="$project_root/_build/Darwin ART Manager.app"
contents="$app/Contents"
binary="$contents/MacOS/DarwinARTManager"
sdk="$(xcrun --sdk macosx --show-sdk-path)"

mkdir -p "$contents/MacOS"
cp "$source_root/Info.plist" "$contents/Info.plist"
xcrun clang -arch arm64 -isysroot "$sdk" -mmacosx-version-min=14.0 \
  -fobjc-arc -fmodules -Wall -Wextra -Werror \
  -framework AppKit -framework UniformTypeIdentifiers \
  "$source_root"/Sources/*.m -o "$binary"
codesign --force --sign - --timestamp=none "$app" >/dev/null
plutil -lint "$contents/Info.plist" >/dev/null

echo "$app"
