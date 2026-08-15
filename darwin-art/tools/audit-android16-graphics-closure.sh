#!/bin/bash
set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
lock_file="$project_root/upstream/android16-graphics-closure-audit.lock"
build_dir="$project_root/_build/graphics-closure-audit"

# The lock is a trusted assignment-only repository file.
# shellcheck disable=SC1090
source "$lock_file"

# mode|expected-members|archive. The order is the actual ld64 order. F means
# force-load module ownership; N means normal provider extraction.
archive_specs=(
  'F|1|_build/android-graphics-jni/libandroid-graphics-layoutlib-registrar-darwin.a'
  'F|61|_build/android-graphics-jni/libandroid-graphics-jni-darwin.a'
  'F|81|_build/hwui-static-foundation/libhwui-static-darwin.a'
  'F|5|_build/hwui-static-foundation/libandroid-graphics-apex-common-darwin.a'
  'F|523|_build/skia-hwui-force-load/libskia.a'
  'N|2|_build/skia-hwui-force-load/libskcms.a'
  'N|36|_build/androidfw-foundation/libandroidfw-darwin.a'
  'N|5|_build/hostgraphics/libhostgraphics-darwin.a'
  'N|36|_build/codec-foundation/libimage_io-darwin.a'
  'N|1|_build/codec-foundation/libmodpb64-darwin.a'
  'N|10|_build/codec-foundation/libultrahdr-darwin.a'
  'N|1|_build/codec-foundation/libjpegencoder-darwin.a'
  'N|1|_build/codec-foundation/libjpegdecoder-darwin.a'
  'N|67|_build/codec-foundation/libjpeg-darwin.a'
  'N|31|_build/minikin-foundation/libminikin.a'
  'N|53|_build/harfbuzz-foundation/libharfbuzz_ng-darwin.a'
  'N|26|_build/graphics-codecs/libft2-darwin.a'
  'N|4|_build/ui-types-foundation/libui-types.a'
  'N|7|_build/nativehelper-foundation/libnativehelper_jvm.a'
  'N|4|_build/nativehelper-foundation/libnativehelper_any_vm.a'
  'N|19|_build/graphics-foundations/libutils-darwin.a'
  'N|8|_build/graphics-foundations/libutils-binder-darwin.a'
  'N|19|_build/graphics-foundations/libcutils-darwin.a'
  'N|8|_build/graphics-foundations/liblog-darwin.a'
  'N|19|_build/libbase-foundation/libandroid-base-darwin.a'
  'N|6|_build/ziparchive-incfs/libziparchive-for-incfs-darwin.a'
  'N|18|_build/graphics-codecs/libpng-darwin.a'
  'N|19|_build/graphics-codecs/libz-darwin.a'
  'N|254|_build/icu-foundation/libicui18n-darwin.a'
  'N|201|_build/icu-foundation/libicuuc-common-darwin.a'
  'F|2|_build/icu-foundation/libandroidicuinit-darwin.a'
  'N|1|_build/icu-foundation/libicuuc-stubdata-darwin.a'
)

if [[ "${#archive_specs[@]}" != "$ARCHIVE_COUNT" ]]; then
  echo "graphics-closure: locked archive count mismatch" >&2
  exit 3
fi

mkdir -p "$build_dir/stage"
stage_dir="$(mktemp -d "$build_dir/stage/audit.XXXXXX")"
cleanup() {
  rm -rf "$stage_dir"
}
trap cleanup EXIT

link_order="$stage_dir/link-order.txt"
all_definitions="$stage_dir/all-provider-definitions.txt"
: > "$link_order"
: > "$all_definitions"

declare -a linker_archives
member_total=0
for spec in "${archive_specs[@]}"; do
  IFS='|' read -r mode expected_members relative <<< "$spec"
  archive="$project_root/$relative"
  if [[ ! -f "$archive" ]]; then
    echo "graphics-closure: required module archive is missing" >&2
    echo "  mode=$mode expected-members=$expected_members" >&2
    echo "  archive=$archive" >&2
    exit 2
  fi
  if [[ "$(file "$archive")" != *"ar archive"* || "$(lipo -archs "$archive")" != arm64 ]]; then
    echo "graphics-closure: archive is not an arm64 static archive: $archive" >&2
    exit 3
  fi
  actual_members="$({ ar -t "$archive" || true; } | grep -v '^__\.SYMDEF' | wc -l | tr -d ' ')"
  if [[ "$actual_members" != "$expected_members" ]]; then
    echo "graphics-closure: archive member identity changed" >&2
    echo "  archive=$relative" >&2
    echo "  expected=$expected_members actual=$actual_members" >&2
    exit 3
  fi
  member_total=$((member_total + actual_members))
  printf '%s %s %s\n' "$mode" "$actual_members" "$relative" >> "$link_order"
  nm -gU "$archive" | \
    awk '$2 ~ /^[TDBSCRGWV]$/ && $3 ~ /^_/ { print $3 }' >> "$all_definitions"
  if [[ "$mode" == F ]]; then
    linker_archives+=( -force_load "$archive" )
  else
    linker_archives+=( "$archive" )
  fi
done

if [[ "$member_total" != "$ARCHIVE_MEMBER_TOTAL" ]]; then
  echo "graphics-closure: aggregate archive member count changed" >&2
  echo "  expected=$ARCHIVE_MEMBER_TOTAL actual=$member_total" >&2
  exit 3
fi

sort -u "$all_definitions" -o "$all_definitions"
definition_count="$(wc -l < "$all_definitions" | tr -d ' ')"
definition_sha="$(shasum -a 256 "$all_definitions" | awk '{print $1}')"
if [[ "$definition_count" != "$GLOBAL_DEFINITION_COUNT" ||
      "$definition_sha" != "$GLOBAL_DEFINITIONS_SHA256" ]]; then
  echo "graphics-closure: provider definition identity changed" >&2
  echo "  expected-count=$GLOBAL_DEFINITION_COUNT actual-count=$definition_count" >&2
  echo "  expected-sha=$GLOBAL_DEFINITIONS_SHA256 actual-sha=$definition_sha" >&2
  exit 3
fi

ld_bin="$(xcrun --find ld)"
cxx="$(command -v clang++ || true)"
[[ -n "$cxx" ]] || { echo "graphics-closure: clang++ is required" >&2; exit 2; }
sdk_root="$(xcrun --sdk macosx --show-sdk-path)"
sdk_version="$(xcrun --sdk macosx --show-sdk-version)"
closure_object="$stage_dir/android16-graphics-closure.o"
"$ld_bin" -r -arch arm64 \
  -platform_version macos "$sdk_version" "$sdk_version" \
  -syslibroot "$sdk_root" \
  "${linker_archives[@]}" -o "$closure_object"
if [[ "$(file "$closure_object")" != *"Mach-O 64-bit object arm64"* ]]; then
  echo "graphics-closure: relocatable closure is not arm64 Mach-O" >&2
  exit 3
fi

relocatable_undefined="$stage_dir/relocatable-undefined-symbols.txt"
nm -u "$closure_object" | awk '$1 ~ /^_/ { print $1 }' | sort -u \
  > "$relocatable_undefined"
undefined_count="$(wc -l < "$relocatable_undefined" | tr -d ' ')"
undefined_sha="$(shasum -a 256 "$relocatable_undefined" | awk '{print $1}')"
if [[ "$undefined_count" != "$RELOCATABLE_UNDEFINED_COUNT" ||
      "$undefined_sha" != "$RELOCATABLE_UNDEFINED_SHA256" ]]; then
  echo "graphics-closure: relocatable unresolved identity changed" >&2
  echo "  expected-count=$RELOCATABLE_UNDEFINED_COUNT actual-count=$undefined_count" >&2
  echo "  expected-sha=$RELOCATABLE_UNDEFINED_SHA256 actual-sha=$undefined_sha" >&2
  exit 3
fi

# If a remaining import exists in any archive already supplied above, the
# provider order or normal-archive extraction is wrong. Never classify that as
# an external/system dependency.
provider_order_leaks="$stage_dir/provider-order-leaks.txt"
comm -12 "$relocatable_undefined" "$all_definitions" > "$provider_order_leaks"
provider_order_leak_count="$(wc -l < "$provider_order_leaks" | tr -d ' ')"
if [[ "$provider_order_leak_count" != "$PROVIDER_ORDER_LEAK_COUNT" ]]; then
  echo "graphics-closure: unresolved symbols already have an in-closure provider" >&2
  sed -n '1,80p' "$provider_order_leaks" >&2
  exit 3
fi

missing_mangled="$stage_dir/missing-skia-android-utils.txt"
grep -E '^__ZN(K)?7android4skia19(BitmapRegionDecoder|FrontBufferedStream)' \
  "$relocatable_undefined" > "$missing_mangled" || true
missing_symbol_count="$(wc -l < "$missing_mangled" | tr -d ' ')"
missing_symbol_sha="$(shasum -a 256 "$missing_mangled" | awk '{print $1}')"
if [[ "$missing_symbol_count" != "$MISSING_SYMBOL_COUNT" ||
      "$missing_symbol_sha" != "$MISSING_SYMBOLS_SHA256" ]]; then
  echo "graphics-closure: known missing Skia android_utils symbol set changed" >&2
  echo "  expected-count=$MISSING_SYMBOL_COUNT actual-count=$missing_symbol_count" >&2
  echo "  expected-sha=$MISSING_SYMBOLS_SHA256 actual-sha=$missing_symbol_sha" >&2
  exit 3
fi

executable_resolved="$stage_dir/resolved-by-executable-providers.txt"
comm -23 "$relocatable_undefined" "$missing_mangled" > "$executable_resolved"
executable_resolved_count="$(wc -l < "$executable_resolved" | tr -d ' ')"
if [[ "$executable_resolved_count" != "$EXECUTABLE_PROVIDER_RESOLVED_COUNT" ]]; then
  echo "graphics-closure: executable-provider classification count changed" >&2
  exit 3
fi

main_source="$stage_dir/closure_main.cpp"
main_object="$stage_dir/closure_main.o"
closure_executable="$stage_dir/android16-graphics-closure"
printf '%s\n' 'int main() { return 0; }' > "$main_source"
"$cxx" -std=c++20 -arch arm64 -mmacosx-version-min="$sdk_version" \
  -c "$main_source" -o "$main_object"

set +e
"$cxx" -arch arm64 -mmacosx-version-min="$sdk_version" \
  "$main_object" "$closure_object" \
  -framework CoreFoundation -framework CoreGraphics -framework ImageIO \
  -framework Foundation -framework AppKit \
  -L/opt/homebrew/lib -llz4 -lz \
  -o "$closure_executable" 2> "$stage_dir/executable-link.err"
executable_link_status=$?
set -e

publish_results() {
  mkdir -p "$build_dir"
  cp "$link_order" "$build_dir/link-order.txt"
  cp "$all_definitions" "$build_dir/all-provider-definitions.txt"
  cp "$closure_object" "$build_dir/android16-graphics-closure.o"
  cp "$relocatable_undefined" "$build_dir/relocatable-undefined-symbols.txt"
  cp "$provider_order_leaks" "$build_dir/provider-order-leaks.txt"
  cp "$missing_mangled" "$build_dir/missing-skia-android-utils.txt"
  cp "$executable_resolved" "$build_dir/resolved-by-executable-providers.txt"
  cp "$stage_dir/executable-link.err" "$build_dir/executable-link.err"
}

if [[ "$executable_link_status" != 0 ]]; then
  executable_missing="$stage_dir/executable-missing-demangled.txt"
  awk '/^  "/ {
         line=$0
         sub(/^  "/, "", line)
         sub(/", referenced from:$/, "", line)
         print line
       }' "$stage_dir/executable-link.err" | sort -u > "$executable_missing"
  executable_missing_count="$(wc -l < "$executable_missing" | tr -d ' ')"
  executable_missing_sha="$(shasum -a 256 "$executable_missing" | awk '{print $1}')"
  cp "$executable_missing" "$stage_dir/missing-module-skia_android_utils.txt"
  if [[ "$executable_missing_count" != "$MISSING_SYMBOL_COUNT" ||
        "$executable_missing_sha" != "$MISSING_DEMANGLED_SHA256" ]]; then
    publish_results
    cp "$executable_missing" "$build_dir/unclassified-executable-missing.txt"
    echo "graphics-closure: executable has an unclassified missing-module set" >&2
    echo "  expected-count=$MISSING_SYMBOL_COUNT actual-count=$executable_missing_count" >&2
    echo "  expected-sha=$MISSING_DEMANGLED_SHA256 actual-sha=$executable_missing_sha" >&2
    exit 3
  fi
  publish_results
  cp "$executable_missing" "$build_dir/missing-module-skia_android_utils.txt"
  echo "graphics-closure: relocatable order complete providers=$definition_count order-leaks=0"
  echo "graphics-closure: executable providers resolved=$executable_resolved_count"
  echo "graphics-closure: blocked missing-module=skia_android_utils symbols=$executable_missing_count" >&2
  echo "  required=$SKIA_PROJECT@$SKIA_REVISION" >&2
  echo "  sources=client_utils/android/BitmapRegionDecoder.cpp,client_utils/android/FrontBufferedStream.cpp" >&2
  exit 2
fi

if [[ "$missing_symbol_count" != 0 || "$MISSING_MODULE_COUNT" != 0 ]]; then
  publish_results
  echo "graphics-closure: executable unexpectedly linked while lock still declares a missing module" >&2
  exit 3
fi

"$closure_executable"
linked_libraries="$(otool -L "$closure_executable")"
if grep -E '(CoreText|libicu|libfreetype|libpng|libfmt|/opt/homebrew/opt/(icu|freetype|libpng|fmt))' \
  <<< "$linked_libraries" >/dev/null; then
  echo "graphics-closure: forbidden host graphics/text dependency linked" >&2
  echo "$linked_libraries" >&2
  exit 3
fi
publish_results
cp "$closure_executable" "$build_dir/android16-graphics-closure"
echo "graphics-closure: executable closure complete archive-members=$member_total"
