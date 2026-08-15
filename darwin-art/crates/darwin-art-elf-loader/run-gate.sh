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

"$clang" "${common_flags[@]}" -Wl,-soname,libdarwin_art_provider.so \
  -Wl,--version-script="$crate_root/fixtures/provider.map" \
  "$crate_root/fixtures/provider.c" -o "$fixture_dir/libdarwin_art_provider.so"
"$clang" "${common_flags[@]}" -O0 -Wl,-z,norelro \
  -Wl,-soname,libdarwin_art_positive.so \
  "$crate_root/fixtures/positive.c" -o "$fixture_dir/positive.so"
"$clang" "${common_flags[@]}" -O0 -Wl,-z,now -Wl,-z,norelro \
  -Wl,-soname,libdarwin_art_import.so "$crate_root/fixtures/import.c" \
  "$fixture_dir/libdarwin_art_provider.so" -o "$fixture_dir/import.so"
"$clang" "${common_flags[@]}" -O0 -Wl,-z,now -Wl,-z,norelro \
  -Wl,-soname,libdarwin_art_weak.so \
  "$crate_root/fixtures/weak.c" -o "$fixture_dir/weak.so"
"$clang" "${common_flags[@]}" -O0 -Wl,-z,lazy -Wl,-z,norelro \
  -Wl,-soname,libdarwin_art_lazy.so "$crate_root/fixtures/import.c" \
  "$fixture_dir/libdarwin_art_provider.so" -o "$fixture_dir/lazy.so"
"$clang" "${common_flags[@]}" -O0 -Wl,-z,now -Wl,-z,relro \
  -Wl,-soname,libdarwin_art_relro.so "$crate_root/fixtures/import.c" \
  "$fixture_dir/libdarwin_art_provider.so" -o "$fixture_dir/relro.so"
"$clang" "${common_flags[@]}" -Wl,-z,norelro -Wl,-soname,libdarwin_art_tls.so \
  "$crate_root/fixtures/tls.c" -o "$fixture_dir/tls.so"

for fixture in positive import weak lazy relro tls; do
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

"$readelf" -l -d -r -V --dyn-syms "$fixture_dir/import.so" > "$fixture_dir/import.readelf.txt"
grep -E '\(NEEDED\).*libdarwin_art_provider\.so' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E 'R_AARCH64_ABS64.*provider_value' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E 'R_AARCH64_GLOB_DAT.*provider_data' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E 'R_AARCH64_JUMP_SLOT.*provider_value' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E 'File: libdarwin_art_provider\.so' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E 'Name: DARWIN_ART_1' "$fixture_dir/import.readelf.txt" >/dev/null
grep -E '\(BIND_NOW\)|NOW' "$fixture_dir/import.readelf.txt" >/dev/null
! grep -E 'GNU_RELRO' "$fixture_dir/import.readelf.txt" >/dev/null
"$readelf" -d -r --dyn-syms "$fixture_dir/weak.so" > "$fixture_dir/weak.readelf.txt"
grep -E 'WEAK.*UND.*optional_provider' "$fixture_dir/weak.readelf.txt" >/dev/null
grep -E 'R_AARCH64_(GLOB_DAT|JUMP_SLOT).*optional_provider' "$fixture_dir/weak.readelf.txt" >/dev/null
"$readelf" -d -r "$fixture_dir/lazy.so" > "$fixture_dir/lazy.readelf.txt"
grep -E 'R_AARCH64_JUMP_SLOT.*provider_value' "$fixture_dir/lazy.readelf.txt" >/dev/null
! grep -E '\(BIND_NOW\)|FLAGS.*NOW' "$fixture_dir/lazy.readelf.txt" >/dev/null
"$readelf" -l "$fixture_dir/relro.so" > "$fixture_dir/relro.readelf.txt"
grep -E 'GNU_RELRO' "$fixture_dir/relro.readelf.txt" >/dev/null
"$readelf" -l "$fixture_dir/tls.so" > "$fixture_dir/tls.readelf.txt"
grep -E '^  TLS ' "$fixture_dir/tls.readelf.txt" >/dev/null

cargo test --manifest-path "$crate_root/Cargo.toml"
cargo run --quiet --release --manifest-path "$crate_root/Cargo.toml" --bin elf-loader-gate -- \
  "$fixture_dir/positive.so" "$fixture_dir/import.so" "$fixture_dir/weak.so" \
  "$fixture_dir/lazy.so" "$fixture_dir/relro.so" "$fixture_dir/tls.so"

cargo build --quiet --release --manifest-path "$crate_root/Cargo.toml" --lib
staticlib="$crate_root/target/release/libdarwin_art_elf_loader.a"
[[ -f "$staticlib" ]] || { echo "elf-loader-gate: staticlib missing" >&2; exit 1; }
xcrun clang -std=c11 -arch arm64 -Wall -Wextra -Werror -x c -fsyntax-only \
  -include "$crate_root/include/darwin_art_elf_loader.h" /dev/null
xcrun clang++ -std=c++17 -arch arm64 -Wall -Wextra -Werror \
  -I "$crate_root/include" "$crate_root/ffi-smoke.mm" "$staticlib" \
  -framework Security -framework CoreFoundation -liconv \
  -o "$fixture_dir/ffi-smoke"
file "$fixture_dir/ffi-smoke" | grep -F 'Mach-O 64-bit executable arm64' >/dev/null
for symbol in darwin_art_elf_load_bytes darwin_art_elf_load_path \
  darwin_art_elf_run_initializers darwin_art_elf_lookup darwin_art_elf_unload; do
  nm -gU "$fixture_dir/ffi-smoke" | grep -E " _${symbol}$" >/dev/null
done
"$fixture_dir/ffi-smoke" "$fixture_dir/positive.so" "$fixture_dir/import.so"
