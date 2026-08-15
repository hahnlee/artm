#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
lock="$root/upstream/android35-libcxx-provider-coverage.lock"

fail() {
  echo "android35-libcxx-provider-coverage: $*" >&2
  exit 2
}

sha() {
  shasum -a 256 "$1" | awk '{print $1}'
}

[[ -f "$lock" ]] || fail "missing lock: $lock"
# shellcheck disable=SC1090
source "$lock"

universe="$root/tools/bionic-libc-leaf-facade/imports/ndk-r28c-api35-arm64-libc.tsv"
allocator="$root/tools/bionic-libc-allocator-facade/imports/ndk-r28c-api35-arm64-allocators.tsv"
filesystem="$root/tools/bionic-fs-facade/manifests/imports.tsv"
time="$root/tools/bionic-time-facade/manifests/imports.tsv"
pthread="$root/tools/android-bionic-pthread-provider/libcxx-pthread-imports.tsv"
process_state="$root/tools/bionic-process-state-facade/manifests/imports.tsv"
stdio="$root/tools/bionic-stdio-facade/manifests/imports.tsv"
locale="$root/tools/bionic-locale-facade/manifests/imports.tsv"
numeric="$root/tools/bionic-numeric-facade/manifests/imports.tsv"
float_conversion="$root/tools/bionic-float-conversion-facade/manifests/imports.tsv"
lifecycle="$root/tools/bionic-dso-lifecycle-facade/manifests/imports.tsv"
leaf_source="$root/tools/bionic-libc-leaf-facade/src/leaf.c"
errno_source="$root/tools/bionic-errno-tls/src/errno_tls.c"
phdr_source="$root/tools/android-dl-iterate-phdr-provider/src/provider.cc"

for file in "$universe" "$allocator" "$filesystem" "$time" "$pthread" "$process_state" "$stdio" "$locale" "$numeric" "$float_conversion" "$lifecycle" \
            "$leaf_source" "$errno_source" "$phdr_source"; do
  [[ -f "$file" ]] || fail "missing provider manifest: $file"
done

[[ "$(sha "$universe")" == "$LIBCXX_LIBC_IMPORTS_SHA256" ]] ||
  fail "libc++ libc import manifest drift"
[[ "$(sha "$allocator")" == "$ALLOCATOR_IMPORTS_SHA256" ]] ||
  fail "allocator import manifest drift"
[[ "$(sha "$filesystem")" == "$FILESYSTEM_IMPORTS_SHA256" ]] ||
  fail "filesystem import manifest drift"
[[ "$(sha "$time")" == "$TIME_IMPORTS_SHA256" ]] ||
  fail "time import manifest drift"
[[ "$(sha "$pthread")" == "$PTHREAD_IMPORTS_SHA256" ]] ||
  fail "pthread import manifest drift"
[[ "$(sha "$process_state")" == "$PROCESS_STATE_IMPORTS_SHA256" ]] ||
  fail "process-state import manifest drift"
[[ "$(sha "$stdio")" == "$STDIO_IMPORTS_SHA256" ]] ||
  fail "stdio import manifest drift"
[[ "$(sha "$locale")" == "$LOCALE_IMPORTS_SHA256" ]] ||
  fail "locale import manifest drift"
[[ "$(sha "$numeric")" == "$NUMERIC_IMPORTS_SHA256" ]] ||
  fail "numeric import manifest drift"
[[ "$(sha "$float_conversion")" == "$FLOAT_CONVERSION_IMPORTS_SHA256" ]] ||
  fail "float conversion import manifest drift"
[[ "$(sha "$lifecycle")" == "$LIFECYCLE_IMPORTS_SHA256" ]] ||
  fail "DSO lifecycle import manifest drift"
[[ "$(sha "$leaf_source")" == "$LEAF_PROVIDER_SOURCE_SHA256" ]] ||
  fail "leaf provider source drift"
[[ "$(sha "$errno_source")" == "$ERRNO_PROVIDER_SOURCE_SHA256" ]] ||
  fail "errno provider source drift"
[[ "$(sha "$phdr_source")" == "$PHDR_PROVIDER_SOURCE_SHA256" ]] ||
  fail "program-header provider source drift"
rg -F 'NameEquals(import_name, "__errno")' "$errno_source" >/dev/null ||
  fail "errno provider no longer owns __errno"
rg -F 'std::strcmp(symbol, "dl_iterate_phdr") != 0' "$phdr_source" >/dev/null ||
  fail "program-header provider no longer owns dl_iterate_phdr"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-libcxx-coverage.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
owners="$tmp/owners.tsv"

awk -F '\t' 'NR > 1 && $3 == "A" { print "leaf\t" $1 }' "$universe" >"$owners"
awk -F '\t' 'NR > 1 { print "allocator\t" $1 }' "$allocator" >>"$owners"
printf 'errno\t__errno\n' >>"$owners"
awk -F '\t' 'NR > 1 { print "filesystem\t" $1 }' "$filesystem" >>"$owners"
awk -F '\t' 'NR > 1 { print "time\t" $1 }' "$time" >>"$owners"
awk -F '\t' 'NR > 1 && $4 == "supported" { print "pthread\t" $1 }' "$pthread" >>"$owners"
awk -F '\t' 'NR > 1 { print "process-state\t" $1 }' "$process_state" >>"$owners"
awk -F '\t' 'NR > 1 && $4 !~ /^rejected-/ { print "stdio\t" $1 }' "$stdio" >>"$owners"
awk -F '\t' 'NR > 1 { print "locale\t" $1 }' "$locale" >>"$owners"
awk -F '\t' 'NR > 1 { print "numeric\t" $1 }' "$numeric" >>"$owners"
awk -F '\t' 'NR > 1 { print "float-conversion\t" $1 }' "$float_conversion" >>"$owners"
awk -F '\t' '{ print "lifecycle\t" $1 }' "$lifecycle" >>"$owners"
printf 'phdr\tdl_iterate_phdr\n' >>"$owners"

LC_ALL=C sort -t $'\t' -k2,2 -k1,1 "$owners" -o "$owners"
cut -f2 "$owners" | uniq -d >"$tmp/duplicate-symbols"
[[ ! -s "$tmp/duplicate-symbols" ]] || {
  cat "$tmp/duplicate-symbols" >&2
  fail "multiple facade owners claim the same libc import"
}

awk -F '\t' 'NR > 1 { print $1 }' "$universe" | LC_ALL=C sort -u >"$tmp/universe-symbols"
cut -f2 "$owners" >"$tmp/owned-symbols"
comm -23 "$tmp/owned-symbols" "$tmp/universe-symbols" >"$tmp/unknown-symbols"
[[ ! -s "$tmp/unknown-symbols" ]] || {
  cat "$tmp/unknown-symbols" >&2
  fail "provider claims a symbol outside the pinned libc++ import set"
}

universe_count="$(wc -l <"$tmp/universe-symbols" | tr -d ' ')"
owned_count="$(wc -l <"$tmp/owned-symbols" | tr -d ' ')"
[[ "$universe_count" == "$EXPECTED_IMPORT_COUNT" ]] || fail "import count drift: $universe_count"
[[ "$owned_count" == "$EXPECTED_OWNED_COUNT" ]] || fail "owned count drift: $owned_count"

awk -F '\t' '{ count[$1]++ } END { for (owner in count) print owner "\t" count[owner] }' "$owners" |
  LC_ALL=C sort >"$tmp/provider-counts"
cat >"$tmp/expected-provider-counts" <<'EOF'
allocator	4
errno	1
filesystem	13
float-conversion	2
leaf	11
lifecycle	2
locale	31
numeric	6
phdr	1
process-state	3
pthread	24
stdio	13
time	3
EOF
diff -u "$tmp/expected-provider-counts" "$tmp/provider-counts" ||
  fail "provider ownership counts drift"

awk -F '\t' 'NR == FNR { owned[$2] = 1; next }
  FNR > 1 { total[$3]++; if ($1 in owned) covered[$3]++ }
  END { for (class in total) print class "\t" covered[class] + 0 "\t" total[class] }' \
  "$owners" "$universe" | LC_ALL=C sort >"$tmp/class-counts"
cat >"$tmp/expected-class-counts" <<'EOF'
A	11	11
B	37	76
C	61	65
D	5	8
EOF
diff -u "$tmp/expected-class-counts" "$tmp/class-counts" ||
  fail "capability-class coverage drift"

echo "android35-libcxx-provider-coverage: PASS imports=$universe_count owned=$owned_count duplicate-owners=0"
echo "providers=leaf:11 allocator:4 errno:1 filesystem:13 time:3 pthread:24 process-state:3 phdr:1 stdio:13 locale:31 numeric:6 float-conversion:2 lifecycle:2"
echo "classes=A:11/11 B:37/76 C:61/65 D:5/8 remaining=46"
echo "scope=standalone-gates-not-yet-one-runtime-namespace"
