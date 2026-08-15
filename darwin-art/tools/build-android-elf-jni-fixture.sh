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
objdump="$toolchain/bin/llvm-objdump"
host_clang="$(xcrun --find clang)"
host_objdump="$(xcrun --find llvm-objdump)"

fail() { echo "android-elf-jni-fixture: $*" >&2; exit 3; }
missing() { echo "android-elf-jni-fixture: missing $*" >&2; exit 2; }

for tool in "$android_clang" "$readelf" "$nm" "$objdump" "$host_clang" "$host_objdump"; do
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
if "$readelf" -d "$output" | grep -E '\((FINI|FINI_ARRAY)\)' >/dev/null; then
  fail "first NativeBridge fixture requires unsupported ELF finalizers"
fi
if "$readelf" -l "$output" | grep -F 'GNU_RELRO' >/dev/null; then
  fail "first loader capability requires RELRO to remain disabled"
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

disassembly="$stage/disassembly.txt"
"$objdump" -d "$output" > "$disassembly"
spill_disassembly="$stage/native-spill-disassembly.txt"
sed -n '/<NativeSpill>:/,/^$/p' "$disassembly" > "$spill_disassembly"
[[ -s "$spill_disassembly" ]] || fail "NativeSpill was not retained for PCS audit"
grep -Eq 'ldr[[:space:]]+x[0-9]+, \[sp\]$' "$spill_disassembly" ||
  fail "NativeSpill Android PCS reference is not at sp+0"
grep -Eq 'ldr[[:space:]]+w[0-9]+, \[sp, #0x8\]$' "$spill_disassembly" ||
  fail "NativeSpill Android PCS f4 is not at sp+8"
grep -Eq 'ldr[[:space:]]+w[0-9]+, \[sp, #0x10\]$' "$spill_disassembly" ||
  fail "NativeSpill Android PCS f5 is not at sp+16"
grep -Eq 'ldr[[:space:]]+x[0-9]+, \[sp, #0x18\]$' "$spill_disassembly" ||
  fail "NativeSpill Android PCS d4 is not at sp+24"
spill_stack_loads="$(grep -Ec 'ldr[[:space:]]+[wx][0-9]+, \[sp(, #[^]]+)?\]$' "$spill_disassembly")"
[[ "$spill_stack_loads" == 4 ]] ||
  fail "NativeSpill has $spill_stack_loads stack loads, expected exactly 4"

darwin_object="$stage/native-fixture-darwin.o"
"$host_clang" -arch arm64 -std=c17 -O2 -fvisibility=hidden \
  -I"$toolchain/sysroot/usr/include" -c "$fixture_root/native_fixture.c" \
  -o "$darwin_object"
darwin_disassembly="$stage/darwin-disassembly.txt"
"$host_objdump" -d "$darwin_object" > "$darwin_disassembly"
darwin_spill="$stage/native-spill-darwin-disassembly.txt"
sed -n '/<_NativeSpill>:/,/^$/p' "$darwin_disassembly" > "$darwin_spill"
[[ -s "$darwin_spill" ]] || fail "Darwin NativeSpill was not retained for PCS audit"
grep -Eq 'ldr[[:space:]]+x[0-9]+, \[sp\]$' "$darwin_spill" ||
  fail "NativeSpill Darwin PCS reference is not at sp+0"
grep -Eq 'ldp[[:space:]]+w[0-9]+, w[0-9]+, \[sp, #0x8\]$' "$darwin_spill" ||
  fail "NativeSpill Darwin PCS f4/f5 pair is not at sp+8/sp+12"
grep -Eq 'ldr[[:space:]]+x[0-9]+, \[sp, #0x10\]$' "$darwin_spill" ||
  fail "NativeSpill Darwin PCS d4 is not at sp+16"
darwin_stack_loads="$(grep -Ec '(ldr[[:space:]]+[wx][0-9]+|ldp[[:space:]]+w[0-9]+, w[0-9]+), \[sp(, #[^]]+)?\]$' "$darwin_spill")"
[[ "$darwin_stack_loads" == 3 ]] ||
  fail "Darwin NativeSpill has $darwin_stack_loads stack loads, expected exactly 3"

mkdir -p "$build_dir"
cp "$output" "$build_dir/libdarwin-art-jni-fixture.so"
fixture_sha="$(shasum -a 256 "$output" | awk '{print $1}')"
fixture_size="$(stat -f '%z' "$output")"
generated_dir="$build_dir/generated"
mkdir -p "$generated_dir"
identity="$stage/darwin_art_elf_jni_fixture_identity.h"
{
  echo '#ifndef DARWIN_ART_ELF_JNI_FIXTURE_IDENTITY_H_'
  echo '#define DARWIN_ART_ELF_JNI_FIXTURE_IDENTITY_H_'
  echo '#include <stddef.h>'
  echo "inline constexpr size_t kDarwinArtElfJniFixtureSize = ${fixture_size}u;"
  echo "inline constexpr char kDarwinArtElfJniFixtureSha256[] = \"${fixture_sha}\";"
  echo 'inline constexpr char kDarwinArtElfJniFixtureSpillSignature[] ='
  echo '    "(ZBCSIJLjava/lang/Object;FDFDFDFDFFD)J";'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureAndroidRefStackOffset = 0u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureAndroidF4StackOffset = 8u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureAndroidF5StackOffset = 16u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureAndroidD4StackOffset = 24u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureDarwinRefStackOffset = 0u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureDarwinF4StackOffset = 8u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureDarwinF5StackOffset = 12u;'
  echo 'inline constexpr size_t kDarwinArtElfJniFixtureDarwinD4StackOffset = 16u;'
  echo '#endif'
} > "$identity"
cp "$identity" "$generated_dir/darwin_art_elf_jni_fixture_identity.h"
echo "android-elf-jni-fixture: PASS exports=JNI_OnLoad+JNI_OnUnload imports=0 fini=0 relro=0 register=GetEnv+FindClass+RegisterNatives methods=8(register+spill+env+narrow+returns) pcs=android(ref@0,f4@8,f5@16,d4@24)+darwin(ref@0,f4@8,f5@12,d4@16) size=$fixture_size sha256=$fixture_sha"
