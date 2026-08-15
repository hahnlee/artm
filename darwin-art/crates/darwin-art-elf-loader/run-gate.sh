#!/bin/bash
set -euo pipefail

crate_root="$(cd "$(dirname "$0")" && pwd)"
sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "$sdk_root" ]]; then
  sdk_root="/Users/${USER:?USER is required}/Library/Android/sdk"
fi

ndk_root="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [[ -z "$ndk_root" ]]; then
  ndk_root="$(find "$sdk_root/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
fi
[[ -d "$ndk_root" ]] || { echo "elf-loader-gate: Android NDK not found" >&2; exit 1; }

clang="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android35-clang"
[[ -x "$clang" ]] || { echo "elf-loader-gate: NDK AArch64 clang missing: $clang" >&2; exit 1; }

fixture_dir="$crate_root/target/fixtures"
mkdir -p "$fixture_dir"

common_flags=(
  -shared -fPIC -O2 -nostdlib -fuse-ld=lld
  -Wl,--hash-style=sysv
  -Wl,--build-id=none
  -Wl,-z,max-page-size=16384
  -Wl,-z,common-page-size=16384
)

"$clang" "${common_flags[@]}" -O0 -Wl,-soname,libdarwin_art_positive.so \
  "$crate_root/fixtures/positive.c" -o "$fixture_dir/positive.so"
"$clang" "${common_flags[@]}" -Wl,-soname,libdarwin_art_import.so \
  "$crate_root/fixtures/import.c" -o "$fixture_dir/import.so"
"$clang" "${common_flags[@]}" -Wl,-soname,libdarwin_art_tls.so \
  "$crate_root/fixtures/tls.c" -o "$fixture_dir/tls.so"

for fixture in positive import tls; do
  file "$fixture_dir/$fixture.so" | grep -F 'ELF 64-bit LSB shared object, ARM aarch64' >/dev/null
done

readelf="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf"
nm="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm"
[[ -x "$readelf" && -x "$nm" ]] || { echo "elf-loader-gate: NDK ELF audit tools missing" >&2; exit 1; }

"$readelf" -l -d -r --dyn-syms "$fixture_dir/positive.so" > "$fixture_dir/positive.readelf.txt"
[[ "$(grep -c 'R_AARCH64_RELATIVE' "$fixture_dir/positive.readelf.txt")" == 2 ]]
grep -E 'INIT_ARRAYSZ.*16 \(bytes\)' "$fixture_dir/positive.readelf.txt" >/dev/null
! grep -E '\(NEEDED\)|R_AARCH64_(JUMP_SLOT|GLOB_DAT)| TLS ' "$fixture_dir/positive.readelf.txt" >/dev/null
"$nm" -D --defined-only "$fixture_dir/positive.so" | grep -E ' T fixture_value$' >/dev/null

"$readelf" -d -r "$fixture_dir/import.so" > "$fixture_dir/import.readelf.txt"
grep -E 'R_AARCH64_JUMP_SLOT.*fixture_missing_import' "$fixture_dir/import.readelf.txt" >/dev/null
"$readelf" -l "$fixture_dir/tls.so" > "$fixture_dir/tls.readelf.txt"
grep -E '^  TLS ' "$fixture_dir/tls.readelf.txt" >/dev/null

cargo test --manifest-path "$crate_root/Cargo.toml"
cargo run --quiet --release --manifest-path "$crate_root/Cargo.toml" --bin elf-loader-gate -- \
  "$fixture_dir/positive.so" "$fixture_dir/import.so" "$fixture_dir/tls.so"
