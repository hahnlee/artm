#!/bin/bash
set -euo pipefail
export LC_ALL=C

project_root="$(cd "$(dirname "$0")/.." && pwd)"
fixture_root="$project_root/probes/android-elf-jni-fixture"
build_dir="$project_root/_build/android-elf-jni-fixture"
sdk_root="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
ndk_revision="28.2.13676358"
ndk="$sdk_root/ndk/$ndk_revision"
toolchain="$ndk/toolchains/llvm/prebuilt/darwin-x86_64"
android_clang="$toolchain/bin/aarch64-linux-android35-clang"
readelf="$toolchain/bin/llvm-readelf"
nm="$toolchain/bin/llvm-nm"

fail() { echo "android-elf-jni-fixture: $*" >&2; exit 3; }
missing() { echo "android-elf-jni-fixture: missing $*" >&2; exit 2; }

for tool in "$android_clang" "$readelf" "$nm"; do
  [[ -x "$tool" ]] || missing "$tool"
done

stage="$(mktemp -d "${TMPDIR:-/tmp}/android-elf-jni-fixture.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
output="$stage/libdarwin-art-jni-fixture.so"

"$android_clang" -std=c17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror \
  -shared -nostdlib -fuse-ld=lld -Wl,--build-id=none -Wl,--hash-style=sysv \
  -Wl,-z,now -Wl,-z,norelro -Wl,-z,max-page-size=16384 \
  -Wl,-soname,libdarwin-art-jni-fixture.so \
  -Wl,--version-script,"$fixture_root/exports.map" \
  "$fixture_root/native_fixture.c" -o "$output"

file "$output" | grep -F 'ELF 64-bit LSB shared object, ARM aarch64' >/dev/null ||
  fail "output is not AArch64 ELF"
if "$readelf" -d "$output" | grep -F '(NEEDED)' >/dev/null; then
  fail "register-only fixture unexpectedly imports a DSO"
fi
[[ "$("$nm" -D --defined-only "$output" | awk '$2 == "T" {print $3}' | paste -sd, -)" == \
   'JNI_OnLoad,JNI_OnUnload' ]] || fail "unexpected dynamic exports"
"$readelf" -d "$output" | grep -F 'BIND_NOW' >/dev/null ||
  fail "fixture must request immediate binding"

relocations="$stage/relocations.txt"
"$readelf" -r "$output" > "$relocations"
if awk '/R_AARCH64_/ && $3 != "R_AARCH64_RELATIVE" {bad=1} END {exit bad}' "$relocations"; then
  :
else
  fail "fixture has an unsupported relocation"
fi
grep -F 'R_AARCH64_JUMP_SLOT' "$relocations" >/dev/null &&
  fail "dependency-free fixture must not contain a PLT relocation"

mkdir -p "$build_dir"
cp "$output" "$build_dir/libdarwin-art-jni-fixture.so"
echo "android-elf-jni-fixture: PASS exports=JNI_OnLoad+JNI_OnUnload imports=0 register=GetEnv+FindClass+RegisterNatives methods=register+spill"
