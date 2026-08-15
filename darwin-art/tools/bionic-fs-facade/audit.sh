#!/bin/bash
set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
lock="$script_dir/sources.lock"
fail() { echo "bionic-fs-facade: $1" >&2; exit 2; }
[[ -f "$lock" ]] || fail 'missing sources.lock'
# shellcheck disable=SC1090
source "$lock"

sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
check_hash() {
  [[ "$(sha256 "$1")" == "$2" ]] || fail "hash mismatch: $1"
}

check_hash "$project_root/crates/darwin-art-fs-broker/Cargo.toml" \
  "$FS_BROKER_CARGO_SHA256"
check_hash "$project_root/crates/darwin-art-fs-broker/src/lib.rs" \
  "$FS_BROKER_SOURCE_SHA256"
check_hash "$project_root/crates/darwin-art-prefix/Cargo.toml" \
  "$PREFIX_CARGO_SHA256"
check_hash "$project_root/crates/darwin-art-prefix/src/lib.rs" \
  "$PREFIX_SOURCE_SHA256"
check_hash "$project_root/tools/bionic-errno-tls/sources.lock" \
  "$BIONIC_ERRNO_LOCK_SHA256"
check_hash "$project_root/tools/bionic-errno-tls/src/errno_tls.c" \
  "$BIONIC_ERRNO_SOURCE_SHA256"
check_hash "$project_root/tools/bionic-errno-tls/generated/darwin_to_android.inc" \
  "$BIONIC_ERRNO_MAPPING_SHA256"
check_hash "$project_root/upstream/android16-os-constants-values.tsv" \
  "$OS_CONSTANTS_VALUES_SHA256"
check_hash "$project_root/tools/bionic-libc-leaf-facade/imports/ndk-r28c-api35-arm64-libc.tsv" \
  "$LIBC_IMPORT_MANIFEST_SHA256"

for symbol in close fstat open openat read; do
  awk -F '\t' -v wanted="$symbol" '$1==wanted && $2=="FUNC" && $3=="B" {found=1} END{exit !found}' \
    "$project_root/tools/bionic-libc-leaf-facade/imports/ndk-r28c-api35-arm64-libc.tsv" ||
    fail "libc import classification drift: $symbol"
done

ndk_root="${ANDROID_NDK_ROOT:-$HOME/Library/Android/sdk/ndk/$NDK_REVISION}"
toolchain="$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/bin"
android_cc="$toolchain/aarch64-linux-android35-clang"
readelf="$toolchain/llvm-readelf"
[[ -x "$android_cc" && -x "$readelf" ]] || fail 'missing pinned NDK toolchain'
check_hash "$ndk_root/source.properties" "$NDK_SOURCE_PROPERTIES_SHA256"
check_hash "$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include/aarch64-linux-android/asm/fcntl.h" \
  "$NDK_ANDROID_ARM64_FCNTL_SHA256"
check_hash "$ndk_root/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/include/sys/stat.h" \
  "$NDK_SYS_STAT_SHA256"

temp_root="$(mktemp -d "${TMPDIR:-/tmp}/bionic-fs-facade.XXXXXX")"
cleanup() {
  if [[ -n "$temp_root" && "$temp_root" == "${TMPDIR:-/tmp}"/bionic-fs-facade.* ]]; then
    find "$temp_root" -depth -delete
  fi
}
trap cleanup EXIT

"$android_cc" -std=c17 -Wall -Wextra -Werror -Wpedantic -fsyntax-only \
  "$script_dir/probes/stat_layout.c"
host_cc="$(xcrun --find clang)"
sdk_root="$(xcrun --sdk macosx --show-sdk-path)"
host_flags=(-arch arm64 -isysroot "$sdk_root" -std=c17 -O2 -Wall -Wextra
            -Werror -Wpedantic -I"$script_dir/include")
"$host_cc" "${host_flags[@]}" -c "$script_dir/src/shims.c" \
  -o "$temp_root/shims.o"
nm -u "$temp_root/shims.o" | sed 's/^[[:space:]]*//' | sort \
  >"$temp_root/undefined"
cat >"$temp_root/expected-undefined" <<'EOF'
___error
_darwin_art_bionic_fs_close_core
_darwin_art_bionic_fs_fstat_core
_darwin_art_bionic_fs_open_core
_darwin_art_bionic_fs_openat_core
_darwin_art_bionic_fs_read_core
EOF
diff -u "$temp_root/expected-undefined" "$temp_root/undefined" ||
  fail 'shim dependency drift'
definitions="$(nm -gU "$temp_root/shims.o")"
for symbol in close fs_resolve fstat open openat read; do
  grep -F " _darwin_art_bionic_$symbol" <<<"$definitions" >/dev/null ||
    fail "missing prefixed definition $symbol"
done
if awk '$2 ~ /^[TDS]$/ {print $3}' <<<"$definitions" |
   grep -Ev '^_darwin_art_bionic_' >/dev/null; then
  fail 'unprefixed global definition escaped filesystem facade'
fi
if rg -n 'dlsym|RTLD_|rename|unlink|socket|O_WRONLY[^\n]*accepted|O_RDWR[^\n]*accepted' \
   "$script_dir/src" "$script_dir/include" "$script_dir/manifests" >/dev/null; then
  fail 'forbidden resolver or writable capability entered facade'
fi

CARGO_TARGET_DIR="$temp_root/cargo-target" cargo test --quiet \
  --manifest-path "$project_root/crates/darwin-art-fs-broker/Cargo.toml"
CARGO_TARGET_DIR="$temp_root/cargo-target" cargo test --quiet \
  --manifest-path "$project_root/crates/darwin-art-prefix/Cargo.toml"
CARGO_TARGET_DIR="$temp_root/cargo-target" cargo test --quiet \
  --manifest-path "$script_dir/Cargo.toml"

fixture="$temp_root/libbionic_fs_fixture.so"
"$android_cc" -std=c17 -O2 -fno-builtin -fPIC -fno-stack-protector \
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -Werror -Wpedantic \
  -shared -nostdlib -Wl,-soname,libbionic_fs_fixture.so -Wl,-z,now \
  -Wl,-z,norelro -Wl,--hash-style=sysv "$script_dir/probes/fixture.c" \
  -o "$fixture"
"$readelf" -h "$fixture" | grep -F 'Machine:                           AArch64' >/dev/null ||
  fail 'fixture is not Android AArch64 ELF'
"$readelf" --dyn-syms --wide "$fixture" >"$temp_root/dynsyms"
awk '$7=="UND" && $8!="" {print $8}' "$temp_root/dynsyms" | sort -u \
  >"$temp_root/fixture-undefined"
cat >"$temp_root/expected-fixture-undefined" <<'EOF'
__errno
close
fstat
open
openat
read
EOF
diff -u "$temp_root/expected-fixture-undefined" "$temp_root/fixture-undefined" ||
  fail 'Android filesystem ELF import namespace drift'
grep -E 'GLOBAL DEFAULT +[0-9]+ bionic_fs_fixture_run$' "$temp_root/dynsyms" >/dev/null ||
  fail 'fixture runner export missing'

mkdir -p "$temp_root/root/etc" "$temp_root/outside"
printf '%s' 'brokered-data' >"$temp_root/root/etc/payload.txt"
printf '%s' 'outside-secret' >"$temp_root/outside/secret"
ln -s "$temp_root/outside/secret" "$temp_root/root/etc/outside-link"
CARGO_TARGET_DIR="$temp_root/cargo-target" cargo run --quiet \
  --manifest-path "$script_dir/Cargo.toml" -- "$fixture" "$temp_root/root"
CARGO_TARGET_DIR="$temp_root/cargo-target" cargo clippy --quiet \
  --manifest-path "$script_dir/Cargo.toml" -- -D warnings
cargo fmt --manifest-path "$script_dir/Cargo.toml" -- --check

echo 'bionic-fs-facade: PASS AndroidELF imports=6 read-only broker+prefix stat128 errno closed-resolver'
