#!/bin/bash
set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
lock_file="$project_root/upstream/android16-libcore-memory.lock"
source_root="$project_root/_aosp/libcore-memory"
art_root="$project_root/_aosp/art-memory-complement"
native_root="$source_root/luni/src/main/native"
build_dir="$project_root/_build/libcore-memory"

# shellcheck disable=SC1090
source "$lock_file"

sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
fail() { echo "libcore-memory: $*" >&2; exit 3; }
blocked() { echo "libcore-memory: BLOCKED: $*" >&2; exit 2; }

materialize() {
  local project="$1" revision="$2" root="$3" relative="$4" expected="$5"
  local destination="$root/$relative"
  if [[ -f "$destination" ]]; then
    [[ "$(sha256 "$destination")" == "$expected" ]] ||
      fail "checksum mismatch: $destination"
    return
  fi
  mkdir -p "$(dirname "$destination")"
  local staged
  staged="$(mktemp "${destination}.download.XXXXXX")"
  curl -fsSL \
    "https://android.googlesource.com/$project/+/$revision/$relative?format=TEXT" \
    | base64 -D > "$staged"
  [[ "$(sha256 "$staged")" == "$expected" ]] || {
    rm -f "$staged"
    fail "download checksum mismatch: $relative"
  }
  mv "$staged" "$destination"
}

materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/libcore_io_Memory.cpp "$MEMORY_CPP_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/java/libcore/io/Memory.java "$MEMORY_JAVA_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/JniConstants.h "$JNI_CONSTANTS_H_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/JniConstants.cpp "$JNI_CONSTANTS_CPP_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/Portability.h "$PORTABILITY_H_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/ScopedBytes.h "$SCOPED_BYTES_H_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/Android.bp "$NATIVE_ANDROID_BP_SHA256"
materialize "$LIBCORE_PROJECT" "$LIBCORE_REVISION" "$source_root" \
  luni/src/main/native/Register.cpp "$REGISTER_CPP_SHA256"

materialize "$ART_PROJECT" "$ART_REVISION" "$art_root" \
  runtime/native/libcore_io_Memory.cc "$ART_MEMORY_CC_SHA256"
materialize "$ART_PROJECT" "$ART_REVISION" "$art_root" \
  runtime/native/libcore_io_Memory.h "$ART_MEMORY_H_SHA256"
materialize "$ART_PROJECT" "$ART_REVISION" "$art_root" \
  runtime/runtime.cc "$ART_RUNTIME_CC_SHA256"
materialize "$ART_PROJECT" "$ART_REVISION" "$art_root" \
  runtime/Android.bp "$ART_RUNTIME_ANDROID_BP_SHA256"

stage="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-libcore-memory.XXXXXX")"
trap 'rm -rf "$stage"' EXIT

python3 - "$native_root/libcore_io_Memory.cpp" \
  "$art_root/runtime/native/libcore_io_Memory.cc" \
  "$source_root/luni/src/main/java/libcore/io/Memory.java" \
  "$stage" "$LIBCORE_METHOD_COUNT" "$LIBCORE_METHOD_MANIFEST_SHA256" \
  "$ART_METHOD_COUNT" "$ART_METHOD_MANIFEST_SHA256" \
  "$COMPLETE_MANAGED_NATIVE_COUNT" <<'PY'
import hashlib
import re
import sys
from pathlib import Path

libcore = Path(sys.argv[1]).read_text()
art = Path(sys.argv[2]).read_text()
java = Path(sys.argv[3]).read_text()
stage = Path(sys.argv[4])

def methods(text):
    start = text.index('static JNINativeMethod gMethods[]')
    body = text[start:text.index('};', start)]
    return re.findall(
        r'(?:FAST_)?NATIVE_METHOD\(Memory,\s*([^,]+),\s*"([^"]+)"\)', body)

def verify(items, count, digest, label):
    manifest = ''.join(f'{name}\t{signature}\n' for name, signature in items)
    if len(items) != int(count):
        raise SystemExit(f'{label} method count drift: {len(items)}')
    if hashlib.sha256(manifest.encode()).hexdigest() != digest:
        raise SystemExit(f'{label} method manifest drift')
    (stage / f'{label}-methods.tsv').write_text(manifest)

libcore_methods = methods(libcore)
art_methods = methods(art)
verify(libcore_methods, sys.argv[5], sys.argv[6], 'libcore')
verify(art_methods, sys.argv[7], sys.argv[8], 'art')
libcore_names = {name for name, _ in libcore_methods}
art_names = {name for name, _ in art_methods}
if libcore_names & art_names:
    raise SystemExit(f'duplicate split owners: {sorted(libcore_names & art_names)}')
native_names = set(re.findall(
    r'\bnative\s+[A-Za-z0-9_<>\[\]]+\s+([A-Za-z0-9_]+)\s*\(', java))
if len(native_names) != int(sys.argv[9]):
    raise SystemExit(f'managed native count drift: {len(native_names)}')
if native_names != libcore_names | art_names:
    raise SystemExit('managed native names differ from ART+libcore union')
PY

grep -F 'libcore_io_Memory.cpp' "$native_root/Android.bp" >/dev/null ||
  fail "Memory left libjavacore source selection"
grep -F 'REGISTER(register_libcore_io_Memory);' \
  "$native_root/Register.cpp" >/dev/null ||
  fail "libcore Memory registration call missing"
grep -F '"native/libcore_io_Memory.cc"' \
  "$art_root/runtime/Android.bp" >/dev/null ||
  fail "ART Memory complement left libart source selection"
grep -F 'register_libcore_io_Memory(env);' \
  "$art_root/runtime/runtime.cc" >/dev/null ||
  fail "ART Memory complement registration call missing"

nativehelper_source="$project_root/_aosp/libnativehelper-full"
nativehelper_archive="$project_root/_build/nativehelper-foundation/libnativehelper_jvm.a"
liblog_include="$project_root/_aosp/system/logging/liblog/include"
liblog_archive="$project_root/_build/graphics-foundations/liblog-darwin.a"
portability="$project_root/probes/android16-memory-portability"
for required in \
  "$nativehelper_source/include_jni/jni.h" \
  "$nativehelper_source/include/nativehelper/JNIHelp.h" \
  "$nativehelper_source/include/nativehelper/ScopedPrimitiveArray.h" \
  "$nativehelper_source/include_platform_header_only/nativehelper/jni_macros.h" \
  "$nativehelper_archive" "$liblog_include/android/log.h" "$liblog_archive" \
  "$portability/byteswap.h" "$portability/sys/sendfile.h" \
  "$project_root/probes/android16_libcore_memory_jni.cc" \
  "$project_root/probes/memory/libcore/io/Memory.java" \
  "$project_root/probes/memory/dev/darwinart/probe/LibcoreMemorySmoke.java"; do
  [[ -e "$required" ]] || blocked "missing build dependency: $required"
done

cc="$(xcrun --find clang++)"
libtool_bin="$(xcrun --find libtool)"
sdk_root="$(xcrun --sdk macosx --show-sdk-path)"
common_flags=(
  -std=c++20 -arch arm64 -isysroot "$sdk_root" -fPIC
  -Wall -Wextra -Werror
  -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare
  -Wno-deprecated-declarations -Wno-macro-redefined
  -I"$portability" -I"$native_root"
  -I"$nativehelper_source/include_jni"
  -I"$nativehelper_source/include"
  -I"$nativehelper_source/include_platform"
  -I"$nativehelper_source/include_platform_header_only"
  -I"$nativehelper_source/header_only_include"
  -I"$liblog_include"
)

memory_object="$stage/libcore_io_Memory.o"
"$cc" "${common_flags[@]}" -c "$native_root/libcore_io_Memory.cpp" \
  -o "$memory_object"
[[ "$(file "$memory_object")" == *"Mach-O 64-bit object arm64"* ]] ||
  fail "Memory object is not Darwin arm64"
archive="$stage/libcore-memory-darwin.a"
"$libtool_bin" -static -o "$archive" "$memory_object"
[[ "$(lipo -archs "$archive")" == arm64 ]] || fail "archive is not arm64"
[[ "$({ ar -t "$archive" || true; } | grep -v '^__\.SYMDEF' | \
      wc -l | tr -d ' ')" == 1 ]] || fail "archive must contain exact one-TU owner"

symbols="$stage/archive-symbols.txt"
nm -g "$archive" > "$symbols"
grep -F 'register_libcore_io_Memory' "$symbols" >/dev/null ||
  fail "complete libcore Memory registrar missing"
for provider in GetPrimitiveByteArrayClass android_log_assert android_log_print; do
  nm -u "$archive" | grep -F "$provider" >/dev/null ||
    fail "expected direct provider edge missing: $provider"
done

jni_constants_object="$stage/JniConstants.o"
"$cc" "${common_flags[@]}" -c "$native_root/JniConstants.cpp" \
  -o "$jni_constants_object"
[[ "$(file "$jni_constants_object")" == *"Mach-O 64-bit object arm64"* ]] ||
  fail "JniConstants provider object is not Darwin arm64"
jni_constants_archive="$stage/libcore-jni-constants-darwin.a"
"$libtool_bin" -static -o "$jni_constants_archive" "$jni_constants_object"
[[ "$(lipo -archs "$jni_constants_archive")" == arm64 ]] ||
  fail "JniConstants provider archive is not arm64"
[[ "$({ ar -t "$jni_constants_archive" || true; } | grep -v '^__\.SYMDEF' | \
      wc -l | tr -d ' ')" == 1 ]] ||
  fail "JniConstants provider must remain an exact one-TU module"
nm -g "$jni_constants_archive" | \
  grep -F 'GetPrimitiveByteArrayClass' >/dev/null ||
  fail "Memory memmove direct provider is missing"

"$cc" "${common_flags[@]}" -c \
  "$project_root/probes/android16_libcore_memory_jni.cc" \
  -o "$stage/android16_libcore_memory_jni.o"
managed_library="$stage/libandroid16-libcore-memory-smoke.dylib"
"$cc" -arch arm64 -isysroot "$sdk_root" -dynamiclib \
  "$stage/android16_libcore_memory_jni.o" -Wl,-force_load,"$archive" \
  -Wl,-force_load,"$jni_constants_archive" \
  -Wl,-force_load,"$nativehelper_archive" "$liblog_archive" \
  -Wl,-exported_symbol,_JNI_OnLoad -Wl,-dead_strip \
  -Wl,-undefined,dynamic_lookup -framework CoreFoundation \
  -o "$managed_library"
otool -L "$managed_library" | grep -F '/opt/homebrew/' >/dev/null &&
  fail "forbidden Homebrew runtime dependency"

classes="$stage/classes"
mkdir -p "$classes"
javac --release 17 -encoding UTF-8 -d "$classes" \
  "$project_root/probes/memory/libcore/io/Memory.java" \
  "$project_root/probes/memory/dev/darwinart/probe/LibcoreMemorySmoke.java"
managed_output="$(java -cp "$classes" \
  dev.darwinart.probe.LibcoreMemorySmoke "$managed_library")"
expected='managed-libcore-memory: methods=18+7 byte=pass unaligned=pass endian=pass arrays=pass bulk=pass'
[[ "$managed_output" == "$expected" ]] ||
  fail "managed acceptance failed: $managed_output"

mkdir -p "$build_dir"
cp "$archive" "$build_dir/libcore-memory-darwin.a"
cp "$jni_constants_archive" "$build_dir/libcore-jni-constants-darwin.a"
echo "libcore-memory: split=ART7+libcore18 union=25 overlap=0 providers=JniConstants+liblog byte=pass unaligned=pass endian=pass arrays=pass bulk=pass archives=Mach-O-arm64"
