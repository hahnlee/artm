#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

root="$(cd "$(dirname "$0")/.." && pwd)"
module="$root/tools/bionic-runtime-provider-closure"
build="$root/_build/bionic-runtime-provider-closure"
objects="$build/objects"
cargo_target="$build/cargo-target"
sdk="$(xcrun --sdk macosx --show-sdk-path)"
cc="$(xcrun --find clang)"
cxx="$(xcrun --find clang++)"
ar="$(xcrun --find ar)"

mkdir -p "$objects"
CARGO_TARGET_DIR="$cargo_target" cargo build --quiet --release \
  --manifest-path "$module/Cargo.toml"
cp "$cargo_target/release/libbionic_runtime_provider_closure.a" \
  "$build/libdarwin-art-bionic-rust-providers.a"
rust="$build/libdarwin-art-bionic-rust-providers.a"
# The standalone Rust facade builds each embed its own test-local errno object.
# The composed runtime owns errno once through the gdtoa archive, so remove all
# bundled copies before the closure is published.
: >"$build/archive-filter.log"
while errno_member="$("$ar" -t "$rust" | grep -E '^(errno|errno_tls)\.o$' | head -n 1)" &&
      [[ -n "$errno_member" ]]; do
  "$ar" -d "$rust" "$errno_member" 2>>"$build/archive-filter.log"
done
if "$ar" -t "$rust" | grep -E '^(errno|errno_tls)\.o$' >/dev/null; then
  echo 'bionic-runtime-provider-closure: Rust errno member filtering failed' >&2
  exit 2
fi
float_target="$build/float-target"
CARGO_TARGET_DIR="$float_target" cargo build --quiet --release \
  --manifest-path "$root/tools/bionic-float-conversion-facade/Cargo.toml"
float_source="$(find "$float_target/release/build" \
  -path '*/out/libdarwin_art_bionic_float_conversion.a' -print -quit)"
[[ -n "$float_source" && -f "$float_source" ]] || {
  echo 'bionic-runtime-provider-closure: float provider archive missing' >&2
  exit 2
}
cp "$float_source" "$build/libdarwin-art-bionic-float-conversion.a"

cflags=(-arch arm64 -isysroot "$sdk" -O2 -Wall -Wextra -Werror -Wpedantic)
cxxflags=(-arch arm64 -isysroot "$sdk" -std=c++20 -O2 -Wall -Wextra -Werror)

"$cc" "${cflags[@]}" -std=c17 -fno-builtin \
  -I"$root/tools/bionic-libc-leaf-facade/include" \
  -c "$root/tools/bionic-libc-leaf-facade/src/leaf.c" -o "$objects/leaf.o"
"$cc" "${cflags[@]}" -std=c17 \
  -I"$root/tools/bionic-libc-allocator-facade/include" \
  -c "$root/tools/bionic-libc-allocator-facade/src/allocator.c" -o "$objects/allocator.o"
"$cc" "${cflags[@]}" -std=c17 \
  -I"$root/tools/bionic-time-facade/include" \
  -c "$root/tools/bionic-time-facade/src/shims.c" -o "$objects/time.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/android-bionic-pthread-provider/include" \
  -c "$root/tools/android-bionic-pthread-provider/src/provider.cc" -o "$objects/pthread.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/android-dl-iterate-phdr-provider/include" \
  -c "$root/tools/android-dl-iterate-phdr-provider/src/provider.cc" -o "$objects/phdr.o"

icu_root="$root/_aosp/external/icu-graphics"
"$cxx" "${cxxflags[@]}" -fno-builtin -fvisibility=hidden -DANDROID \
  -I"$root/tools/bionic-locale-facade/include" \
  -I"$icu_root/android_icu4c/include" \
  -I"$icu_root/icu4c/source/common" \
  -I"$icu_root/libandroidicuinit/include" \
  -c "$root/tools/bionic-locale-facade/src/provider.cc" -o "$objects/locale.o"
"$cc" "${cflags[@]}" -std=c17 -fno-builtin \
  -I"$root/tools/bionic-numeric-facade/include" \
  -c "$root/tools/bionic-numeric-facade/src/provider.c" -o "$objects/numeric.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/bionic-format-facade/include" \
  -I"$root/tools/bionic-libc-allocator-facade/include" \
  -I"$root/tools/bionic-errno-tls/include" \
  -c "$root/tools/bionic-format-facade/src/format.cc" -o "$objects/format.o"
"$cc" -arch arm64 -isysroot "$sdk" \
  -c "$root/tools/bionic-format-facade/src/aapcs64_entry.S" -o "$objects/format-entry.o"
"$cxx" "${cxxflags[@]}" -fno-builtin \
  -I"$root/tools/bionic-formatted-stdio-facade/include" \
  -I"$root/tools/bionic-format-facade/include" \
  -I"$root/tools/bionic-stdio-facade/include" \
  -I"$root/tools/bionic-libc-allocator-facade/include" \
  -I"$root/tools/bionic-errno-tls/include" \
  -c "$root/tools/bionic-formatted-stdio-facade/src/provider.cc" \
  -o "$objects/formatted-stdio.o"
"$cc" -arch arm64 -isysroot "$sdk" \
  -c "$root/tools/bionic-formatted-stdio-facade/src/aapcs64_entry.S" \
  -o "$objects/formatted-stdio-entry.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/bionic-syslog-facade/include" \
  -I"$root/tools/bionic-format-facade/include" \
  -I"$root/tools/bionic-libc-allocator-facade/include" \
  -I"$root/tools/bionic-errno-tls/include" \
  -c "$root/tools/bionic-syslog-facade/src/syslog.cc" -o "$objects/syslog.o"
"$cc" -arch arm64 -isysroot "$sdk" \
  -c "$root/tools/bionic-syslog-facade/src/aapcs64_entry.S" -o "$objects/syslog-entry.o"
"$cc" "${cflags[@]}" -std=c17 -fno-builtin \
  -I"$root/tools/bionic-strerror-facade/include" \
  -I"$root/tools/bionic-strerror-facade/generated" \
  -c "$root/tools/bionic-strerror-facade/src/strerror.c" -o "$objects/strerror.o"
"$cc" "${cflags[@]}" -std=c17 -fno-builtin \
  -I"$root/tools/bionic-wide-integer-facade/include" \
  -c "$root/tools/bionic-wide-integer-facade/src/provider.c" -o "$objects/wide-integer.o"
"$cxx" "${cxxflags[@]}" -fno-builtin -fvisibility=hidden -DANDROID \
  -I"$root/tools/bionic-wide-float-facade/include" \
  -I"$root/tools/bionic-float-conversion-facade/include" \
  -I"$root/tools/bionic-libc-allocator-facade/include" \
  -I"$root/tools/bionic-errno-tls/include" \
  -I"$icu_root/android_icu4c/include" \
  -I"$icu_root/icu4c/source/common" \
  -I"$icu_root/libandroidicuinit/include" \
  -c "$root/tools/bionic-wide-float-facade/src/provider.cc" \
  -o "$objects/wide-float.o"
"$cc" "${cflags[@]}" -std=c17 -fno-builtin \
  -I"$root/tools/bionic-abort-facade/include" \
  -c "$root/tools/bionic-abort-facade/src/provider.c" -o "$objects/abort.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/android-liblog-exec-provider/include" \
  -I"$root/_aosp/system/logging/liblog/include" \
  -c "$root/tools/android-liblog-exec-provider/liblog_provider.cc" -o "$objects/liblog.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/bionic-provider-namespace/include" \
  -I"$root/tools/bionic-provider-namespace/generated" \
  -c "$root/tools/bionic-provider-namespace/src/namespace.cc" -o "$objects/namespace.o"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/bionic-provider-namespace/include" \
  -c "$root/tools/bionic-provider-namespace/src/builtin_adapters.cc" \
  -o "$objects/builtin_adapters.o"

native="$build/libdarwin-art-bionic-native-providers.a"
"$ar" rcs "$native" \
  "$objects/leaf.o" "$objects/allocator.o" "$objects/time.o" \
  "$objects/pthread.o" "$objects/phdr.o" "$objects/locale.o" \
  "$objects/numeric.o" "$objects/format.o" "$objects/format-entry.o" \
  "$objects/formatted-stdio.o" "$objects/formatted-stdio-entry.o" \
  "$objects/syslog.o" "$objects/syslog-entry.o" \
  "$objects/strerror.o" "$objects/wide-integer.o" "$objects/wide-float.o" \
  "$objects/abort.o" \
  "$objects/liblog.o" "$objects/namespace.o" \
  "$objects/builtin_adapters.o"

float="$build/libdarwin-art-bionic-float-conversion.a"
icu="$root/_build/icu-foundation"
smoke="$build/full-link-smoke"
"$cxx" "${cxxflags[@]}" \
  -I"$root/tools/bionic-provider-namespace/include" \
  -I"$root/tools/bionic-provider-namespace/generated" \
  -I"$root/_aosp/system/logging/liblog/include" \
  "$module/full_link_smoke.cc" "$native" "$float" "$rust" \
  -Wl,-force_load,"$icu/libandroidicuinit-darwin.a" \
  "$icu/libicuuc-common-darwin.a" "$icu/libicuuc-stubdata-darwin.a" \
  "$root/_build/graphics-foundations/liblog-darwin.a" \
  -framework Security -o "$smoke"

"$smoke"
symbols="$build/provider-resolvers.txt"
nm -gU "$native" "$float" "$rust" > "$build/all-symbols.txt" 2>/dev/null
cat > "$symbols" <<'EOF'
_darwin_art_bionic_abort_resolve
_darwin_art_bionic_allocator_resolve
_darwin_art_bionic_dso_lifecycle_resolve
_darwin_art_bionic_errno_resolve
_darwin_art_bionic_float_conversion_resolve
_darwin_art_bionic_format_resolve
_darwin_art_bionic_formatted_stdio_resolve
_darwin_art_bionic_fs_resolve
_darwin_art_bionic_libc_leaf_resolve
_darwin_art_bionic_locale_resolve
_darwin_art_bionic_numeric_resolve
_darwin_art_bionic_process_state_resolve
_darwin_art_bionic_pthread_resolve
_darwin_art_bionic_stdio_resolve
_darwin_art_bionic_strerror_resolve
_darwin_art_bionic_syslog_resolve
_darwin_art_bionic_time_resolve
_darwin_art_bionic_wide_float_resolve
_darwin_art_bionic_wide_integer_resolve
_darwin_art_dl_phdr_resolve
_darwin_art_liblog_provider_resolve
EOF
while IFS= read -r symbol; do
  grep -E " [TDS] ${symbol}$" "$build/all-symbols.txt" >/dev/null || {
    echo "bionic-runtime-provider-closure: missing resolver $symbol" >&2
    exit 2
  }
done < "$symbols"
nm -gU "$native" "$float" "$rust" 2>/dev/null |
  awk '$2 ~ /^[TDS]$/ && $3 ~ /^_darwin_art_/ {print $3}' |
  sort | uniq -d > "$build/duplicate-provider-definitions.txt"
[[ ! -s "$build/duplicate-provider-definitions.txt" ]] || {
  cat "$build/duplicate-provider-definitions.txt" >&2
  echo 'bionic-runtime-provider-closure: duplicate provider definitions' >&2
  exit 2
}
nm -gU "$native" "$float" "$rust" \
  "$icu/libandroidicuinit-darwin.a" "$icu/libicuuc-common-darwin.a" \
  "$icu/libicuuc-stubdata-darwin.a" \
  "$root/_build/graphics-foundations/liblog-darwin.a" \
  > "$build/ownership-symbols.txt" 2>/dev/null
for symbol in _darwin_art_bionic_malloc_result _darwin_art_bionic_free \
              _darwin_art_bionic_strtod _darwin_art_bionic_strtof \
              _darwin_art_bionic___errno _darwin_art_bionic_errno_store \
              _darwin_art_bionic_format_resolve \
              _darwin_art_bionic_formatted_stdio_resolve \
              _darwin_art_bionic_fprintf _darwin_art_bionic_vfprintf \
              _darwin_art_bionic_stdio_fwrite_core \
              _darwin_art_bionic_syslog_resolve \
              _darwin_art_liblog_provider_resolve ___android_log_write \
              _darwin_art_bionic_wide_float_resolve __Z16android_icu_initv \
              _u_hasBinaryProperty_76; do
  [[ "$(grep -Ec " [TDS] ${symbol}$" "$build/ownership-symbols.txt")" == 1 ]] || {
    echo "bionic-runtime-provider-closure: non-unique provider $symbol" >&2
    exit 2
  }
done
if otool -L "$smoke" | grep -E '(/opt/homebrew|/usr/local|libicu(uc|i18n))' >/dev/null; then
  echo 'bionic-runtime-provider-closure: host/dynamic ICU escaped' >&2
  exit 2
fi
echo 'bionic-runtime-provider-closure: PASS providers=21 bind_builtins=sealed routes=165 Rust+C+C++=linked duplicate-provider=0 host-fallback=0'
