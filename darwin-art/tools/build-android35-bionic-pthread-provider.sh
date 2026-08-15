#!/bin/bash
set -euo pipefail
export LC_ALL=C

project_root="$(cd "$(dirname "$0")/.." && pwd)"
source "$project_root/upstream/android35-bionic-pthread-provider.lock"
module_root="$project_root/tools/android-bionic-pthread-provider"
runner_root="$project_root/tools/android-bionic-pthread-provider-runner"
source_root="$project_root/_aosp/bionic-pthread-provider"
build_dir="$project_root/_build/bionic-pthread-provider"
ndk_root="${ANDROID_NDK_ROOT:-$HOME/Library/Android/sdk/ndk/$NDK_REVISION}"
toolchain="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64"
libcxx="$toolchain/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
pthread_types_h="$toolchain/sysroot/usr/include/bits/pthread_types.h"
pthread_h="$toolchain/sysroot/usr/include/pthread.h"
readelf="$toolchain/bin/llvm-readelf"
elf_nm="$toolchain/bin/llvm-nm"
android_clang="$toolchain/bin/aarch64-linux-android${ANDROID_API}-clang"

fail() { echo "bionic-pthread-provider: $*" >&2; exit 3; }
missing() { echo "bionic-pthread-provider: missing $*" >&2; exit 2; }
sha() { shasum -a 256 "$1" | awk '{print $1}'; }

for input in "$libcxx" "$pthread_types_h" "$pthread_h"; do
  [[ -f "$input" ]] || missing "$input"
done
for tool in "$readelf" "$elf_nm" "$android_clang"; do
  [[ -x "$tool" ]] || missing "$tool"
done
[[ "$(stat -f '%z' "$libcxx")" == "$NDK_LIBCXX_SHARED_SIZE" ]] ||
  fail "libc++_shared size mismatch"
[[ "$(sha "$libcxx")" == "$NDK_LIBCXX_SHARED_SHA256" ]] ||
  fail "libc++_shared SHA mismatch"
[[ "$(sha "$pthread_types_h")" == "$NDK_PTHREAD_TYPES_H_SHA256" ]] ||
  fail "NDK pthread_types.h SHA mismatch"
[[ "$(sha "$pthread_h")" == "$NDK_PTHREAD_H_SHA256" ]] ||
  fail "NDK pthread.h SHA mismatch"

stage="$(mktemp -d "${TMPDIR:-/tmp}/bionic-pthread-provider.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
"$readelf" --dyn-syms --wide "$libcxx" |
  awk '$7=="UND" && $8 ~ /^pthread_.*@LIBC/ {
         name=$8; sub(/@.*/, "", name);
         version=$8; sub(/^[^@]*@/, "", version);
         print name "\t" $4 "\t" version
       }' | sort -u > "$stage/libcxx-pthread-imports.tsv"
tail -n +2 "$module_root/libcxx-pthread-imports.tsv" | cut -f1-3 > "$stage/locked-imports.tsv"
diff -u "$stage/locked-imports.tsv" "$stage/libcxx-pthread-imports.tsv" ||
  fail "libc++ pthread import manifest drift"
[[ "$(wc -l < "$stage/libcxx-pthread-imports.tsv" | tr -d ' ')" == "$LIBCXX_PTHREAD_IMPORT_COUNT" ]] ||
  fail "pthread import count mismatch"
[[ "$(awk -F '\t' '$4=="supported"{n++}END{print n+0}' "$module_root/libcxx-pthread-imports.tsv")" == "$SUPPORTED_PTHREAD_IMPORT_COUNT" ]] ||
  fail "supported import count mismatch"
[[ "$(awk -F '\t' '$4=="unsupported"{n++}END{print n+0}' "$module_root/libcxx-pthread-imports.tsv")" == "$UNSUPPORTED_PTHREAD_IMPORT_COUNT" ]] ||
  fail "unsupported import count mismatch"

files=(
  libc/bionic/pthread_once.cpp
  libc/bionic/pthread_mutex.cpp
  libc/bionic/pthread_key.cpp
  libc/bionic/pthread_self.cpp
)
hashes=(
  "$BIONIC_PTHREAD_ONCE_CPP_SHA256"
  "$BIONIC_PTHREAD_MUTEX_CPP_SHA256"
  "$BIONIC_PTHREAD_KEY_CPP_SHA256"
  "$BIONIC_PTHREAD_SELF_CPP_SHA256"
)
[[ "${#files[@]}" == "$LOCKED_BIONIC_SOURCE_COUNT" ]] ||
  fail "locked Bionic source count mismatch"
for index in "${!files[@]}"; do
  relative="${files[$index]}"
  destination="$source_root/$relative"
  expected="${hashes[$index]}"
  if [[ ! -f "$destination" ]]; then
    mkdir -p "$(dirname "$destination")"
    staged="$(mktemp "${TMPDIR:-/tmp}/bionic-pthread-source.XXXXXX")"
    curl -fsSL "https://android.googlesource.com/$BIONIC_PROJECT/+/$BIONIC_REVISION/$relative?format=TEXT" |
      base64 -D > "$staged"
    [[ "$(sha "$staged")" == "$expected" ]] ||
      fail "download SHA mismatch: $relative"
    mv "$staged" "$destination"
  fi
  [[ "$(sha "$destination")" == "$expected" ]] ||
    fail "Bionic source SHA mismatch: $relative"
done
printf '%s\n' "$BIONIC_REVISION" > "$source_root/.source-revision"
[[ -z "$(find "$source_root" \( -name .git -o -name .gitmodules \) -print -quit)" ]] ||
  fail "Git metadata is forbidden in sparse source"

python3 - "$source_root" "$pthread_types_h" "$pthread_h" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
types = Path(sys.argv[2]).read_text()
header = Path(sys.argv[3]).read_text()
once = (root / "libc/bionic/pthread_once.cpp").read_text()
mutex = (root / "libc/bionic/pthread_mutex.cpp").read_text()
key = (root / "libc/bionic/pthread_key.cpp").read_text()
self_source = (root / "libc/bionic/pthread_self.cpp").read_text()

assert "typedef long pthread_t;" in types
assert "typedef int pthread_key_t;" in types
assert "typedef int pthread_once_t;" in types
assert "int32_t __private[10]" in types
assert "#define PTHREAD_MUTEX_INITIALIZER { { ((PTHREAD_MUTEX_NORMAL & 3) << 14) } }" in header
assert "#define PTHREAD_ONCE_INIT 0" in header
assert "ONCE_INITIALIZATION_NOT_YET_STARTED   0" in once
assert "ONCE_INITIALIZATION_UNDERWAY          1" in once
assert "ONCE_INITIALIZATION_COMPLETE          2" in once
assert "KEY_VALID_FLAG (1 << 31)" in key
assert "*key = i | KEY_VALID_FLAG;" in key
assert "pthread_key_clean_all" in key and "PTHREAD_DESTRUCTOR_ITERATIONS" in key
assert "key_data[i].data = nullptr" in key
assert "static inline __always_inline bool IsMutexDestroyed" in mutex
assert "mutex_state == 0xffff" in mutex
assert "memset(mutex, 0, sizeof(pthread_mutex_internal_t))" in mutex
assert "return reinterpret_cast<pthread_t>(__get_thread())" in self_source
print("bionic-pthread-provider: upstream-layout=PASS sources=4")
PY

[[ "$(sha "$module_root/fixture/pthread_fixture.c")" == "$FIXTURE_C_SHA256" ]] ||
  fail "fixture source SHA mismatch"
[[ "$(sha "$module_root/fixture/exports.map")" == "$FIXTURE_EXPORTS_SHA256" ]] ||
  fail "fixture exports SHA mismatch"

cxx="$(xcrun --find clang++)"
ar="$(xcrun --find ar)"
host_nm="$(xcrun --find nm)"
macos_sdk="$(xcrun --sdk macosx --show-sdk-path)"
host_flags=(-std=c++20 -arch arm64 -isysroot "$macos_sdk" -O2 -Wall -Wextra -Werror
            -fvisibility=hidden -fvisibility-inlines-hidden -I"$module_root/include")
"$cxx" "${host_flags[@]}" -c "$module_root/src/provider.cc" -o "$stage/provider.o"
if grep -E 'reinterpret_cast<[^>]*pthread_(t|key_t|once_t|mutex_t)' \
    "$module_root/src/provider.cc" >/dev/null; then
  fail "Android opaque object is reinterpreted as a Darwin pthread type"
fi
"$ar" rcs "$stage/libdarwin-art-bionic-pthread.a" "$stage/provider.o"
file "$stage/provider.o" | grep -F 'Mach-O 64-bit object arm64' >/dev/null ||
  fail "provider object is not Darwin arm64"
if "$host_nm" -gU "$stage/provider.o" | awk '{print $3}' |
    grep -E '^_pthread_' >/dev/null; then
  fail "unprefixed pthread definition escaped provider"
fi
for symbol in self key_create key_delete getspecific setspecific once \
              mutex_init mutex_lock mutex_trylock mutex_unlock mutex_destroy; do
  "$host_nm" -gU "$stage/provider.o" | awk '{print $3}' |
    grep -Fx "_darwin_art_bionic_pthread_$symbol" >/dev/null ||
    fail "missing provider definition: pthread_$symbol"
done

fixture="$stage/libdarwin-art-pthread-fixture.so"
"$android_clang" -std=c17 -O2 -mno-outline-atomics -fPIC \
  -fvisibility=hidden -Wall -Wextra -Werror -shared -nostdlib -fuse-ld=lld \
  -Wl,--build-id=none -Wl,--hash-style=sysv -Wl,-z,now -Wl,-z,norelro \
  -Wl,-z,max-page-size=16384 -Wl,-soname,libdarwin-art-pthread-fixture.so \
  -Wl,--version-script,"$module_root/fixture/exports.map" \
  "$module_root/fixture/pthread_fixture.c" -lc -o "$fixture"
file "$fixture" | grep -F 'ELF 64-bit LSB shared object, ARM aarch64' >/dev/null ||
  fail "fixture is not Android arm64 ELF"
[[ "$(sha "$fixture")" == "$FIXTURE_ELF_SHA256" ]] || fail "fixture ELF SHA mismatch"
[[ "$($readelf -d "$fixture" | awk '/\(NEEDED\)/{gsub(/[][]/,"",$NF); print $NF}')" == "libc.so" ]] ||
  fail "fixture namespace is not exactly libc.so"
"$readelf" --dyn-syms --wide "$fixture" |
  awk '$7=="UND" && $8 ~ /^pthread_.*@LIBC/ {name=$8; sub(/@.*/,"",name); print name}' |
  sort -u > "$stage/fixture-imports.txt"
awk -F '\t' '$4=="supported"{print $1}' "$module_root/libcxx-pthread-imports.tsv" |
  sort -u > "$stage/supported-imports.txt"
diff -u "$stage/supported-imports.txt" "$stage/fixture-imports.txt" ||
  fail "fixture does not execute exact supported import set"
[[ "$($elf_nm -D --defined-only "$fixture" | awk '$2=="T"{n++}END{print n+0}')" == 5 ]] ||
  fail "fixture export count mismatch"

export DARWIN_ART_PTHREAD_PROVIDER_LIBDIR="$stage"
export CARGO_TARGET_DIR="$stage/cargo-target"
cargo build --quiet --manifest-path "$runner_root/Cargo.toml"
runner="$CARGO_TARGET_DIR/debug/android-bionic-pthread-provider-runner"
output="$("$runner" "$fixture")"
grep -F 'ELF=executed resolver=libc.so@LIBC imports=11' <<< "$output" >/dev/null ||
  fail "closed resolver execution failed"
grep -F 'tls-destructor=2-pass once=1 mutex=8x2000-contention' <<< "$output" >/dev/null ||
  fail "thread/TLS/once/mutex E2E failed"
grep -F 'destroy-lookup=race-safe' <<< "$output" >/dev/null ||
  fail "destroy/lookup lifecycle synchronization failed"
grep -F 'no-Darwin-reinterpret stale-key=reuse-alias(Bionic-undefined) unsupported=fork+robust+pshared+PI+recursive+errorcheck' <<< "$output" >/dev/null ||
  fail "capability matrix failed"

"$cxx" "${host_flags[@]}" -fsanitize=address,undefined \
  "$module_root/src/provider.cc" "$module_root/tls_delete_stress.cc" \
  -o "$stage/tls-delete-stress"
tls_stress_output="$("$stage/tls-delete-stress")"
grep -F 'delete-vs-get+set ASan=clean' <<< "$tls_stress_output" >/dev/null ||
  fail "TLS delete/get/set sanitizer stress failed"
grep -F 'repeated-delete=10000 peak-cells=1 reset-cells=0' <<< "$tls_stress_output" >/dev/null ||
  fail "TLS repeated-delete bounded-state/reset gate failed"

mkdir -p "$build_dir"
cp "$stage/libdarwin-art-bionic-pthread.a" "$build_dir/"
cp "$fixture" "$build_dir/"
cp "$runner" "$build_dir/"
printf '%s\n' "$output"
printf '%s\n' "$tls_stress_output"
echo "bionic-pthread-provider: PASS imports=11/24 ELF=Android-arm64 host=Darwin-arm64 runtime-files-modified=0"
