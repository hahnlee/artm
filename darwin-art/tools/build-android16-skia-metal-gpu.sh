#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$script_dir/.." && pwd)"
skia="$root/_aosp/external/skia"
build="$root/_build/skia-metal-gpu"
probe="$build/skia-metal-gpu-smoke"

fail() { echo "skia-metal-gpu: $1" >&2; exit 2; }

[[ "$(uname -s)" == Darwin ]] || fail "requires macOS"
[[ -f "$skia/BUILD.gn" ]] || fail "missing materialized Skia"
[[ "$(tr -d '[:space:]' < "$skia/.source-revision")" == \
   "bcb0f77c44783b1800ba37641ba7ecab04f05e07" ]] || fail "Skia revision drift"
[[ -x "$skia/bin/gn" && -x "$skia/third_party/ninja/ninja" ]] ||
  fail "missing pinned GN/Ninja"

sdk="$(xcrun --sdk macosx --show-sdk-path)"
mkdir -p "$build"
args="is_official_build=true is_debug=false target_cpu=\"arm64\" skia_enable_gpu=true skia_enable_graphite=false skia_enable_pdf=false skia_enable_skottie=false skia_enable_svg=false skia_enable_precompile=false skia_enable_tools=false skia_enable_fontmgr_empty=true skia_use_expat=false skia_use_fonthost_mac=false skia_use_freetype=false skia_use_fontconfig=false skia_use_harfbuzz=false skia_use_icu=false skia_use_perfetto=false skia_disable_tracing=false skia_use_gl=false skia_use_metal=true skia_use_vulkan=false skia_use_libjpeg_turbo_decode=false skia_use_libjpeg_turbo_encode=false skia_use_no_jpeg_encode=true skia_use_libpng_decode=false skia_use_libpng_encode=false skia_use_no_png_encode=true skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_no_webp_encode=true skia_use_wuffs=false skia_use_piex=false skia_use_xps=false skia_use_zlib=false skia_use_dng_sdk=false skia_use_libheif=false skia_use_crabbyavif=false skia_use_libjxl_decode=false skia_use_libavif=false skia_use_bidi=false skia_use_libgrapheme=false skia_build_rust_targets=false extra_cflags=[\"-DSK_BUILD_FOR_ANDROID_FRAMEWORK\",\"-DSK_USER_CONFIG_HEADER=\\\"include/config/SkUserConfigManual.h\\\"\",\"-I$root/_aosp/system/logging/liblog/include\",\"-I$root/_aosp/system/core/libcutils/include\"]"

"$skia/bin/gn" gen "$build" --args="$args" --root="$skia"
"$skia/third_party/ninja/ninja" -C "$build" skia

archive="$build/libskia.a"
skcms="$build/libskcms.a"
[[ -f "$archive" && -f "$skcms" ]] || fail "GPU Skia archives missing"

clang++ -std=c++20 -O2 -DNDEBUG -arch arm64 -isysroot "$sdk" \
  -fobjc-arc -Wall -Wextra -Werror \
  '-DSK_USER_CONFIG_HEADER="include/config/SkUserConfigManual.h"' \
  -I"$root/_aosp/system/logging/liblog/include" \
  -I"$root/_aosp/system/core/libcutils/include" \
  -I"$skia" -I"$skia/include" \
  "$root/probes/skia_metal_gpu_smoke.mm" "$archive" "$skcms" \
  "$root/_build/graphics-foundations/liblog-darwin.a" \
  -framework AppKit -framework CoreGraphics -framework CoreFoundation \
  -framework Metal -framework QuartzCore -o "$probe"

output="$(DARWIN_ART_SKIA_METAL_HEADLESS=1 "$probe")"
expected_prefix="skia-metal-gpu: frames=8 cpu-readback=0 full-frame-blits=0"
[[ "$output" == "$expected_prefix"* ]] || fail "unexpected smoke output: $output"
echo "$output"
echo "skia-metal-gpu: archive=$archive"
