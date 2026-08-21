#!/bin/bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
aosp_root="$project_root/_aosp"
hwui="$aosp_root/frameworks/base/libs/hwui"
lock_file="$project_root/upstream/android16-hwui-static-foundation.lock"
output_dir="$project_root/_build/hwui-static-foundation"

# shellcheck disable=SC1090
source "$lock_file"

deps_root="$aosp_root/hwui-static-deps"
if [[ -z "${DARWIN_ART_ANDROID16_SYSPROP_ROOT:-}" ||
      -z "${DARWIN_ART_ANDROID16_SYSPROP_CPP:-}" ||
      -z "${DARWIN_ART_ANDROID16_GUI_INCLUDE:-}" ||
      -z "${DARWIN_ART_ANDROID16_BINDER_INCLUDE:-}" ||
      -z "${DARWIN_ART_ANDROID16_NATIVEDISPLAY_INCLUDE:-}" ||
      -z "${DARWIN_ART_ANDROID16_LIBHARDWARE_INCLUDE:-}" ]]; then
  "$script_dir/sync-android16-hwui-static-deps.sh"
fi

fail_source() {
  echo "hwui-static-foundation: $1" >&2
  exit 2
}

verify_sha() {
  local file="$1" expected="$2"
  [[ -f "$file" ]] || fail_source "missing source $file"
  local actual
  actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  [[ "$actual" == "$expected" ]] ||
    fail_source "checksum mismatch file=$file expected=$expected actual=$actual"
}

verify_sha "$hwui/Android.bp" "$HWUI_ANDROID_BP_SHA256"
verify_sha "$hwui/HWUIProperties.sysprop" "$HWUI_SYSPROP_SHA256"
verify_sha "$project_root/patches/frameworks-base/0001-darwin-android-critical-jni-abi.patch" \
  "$CRITICAL_JNI_PATCH_SHA256"

sources=(
  canvas/CanvasFrontend.cpp
  canvas/CanvasOpBuffer.cpp
  canvas/CanvasOpRasterizer.cpp
  effects/StretchEffect.cpp
  effects/GainmapRenderer.cpp
  pipeline/skia/BackdropFilterDrawable.cpp
  pipeline/skia/HolePunch.cpp
  pipeline/skia/SkiaCpuPipeline.cpp
  pipeline/skia/SkiaDisplayList.cpp
  pipeline/skia/SkiaPipeline.cpp
  pipeline/skia/SkiaRecordingCanvas.cpp
  pipeline/skia/StretchMask.cpp
  pipeline/skia/RenderNodeDrawable.cpp
  pipeline/skia/ReorderBarrierDrawables.cpp
  pipeline/skia/TransformCanvas.cpp
  renderstate/RenderState.cpp
  renderthread/CanvasContext.cpp
  renderthread/DrawFrameTask.cpp
  renderthread/Frame.cpp
  renderthread/RenderEffectCapabilityQuery.cpp
  renderthread/RenderProxy.cpp
  renderthread/RenderTask.cpp
  renderthread/TimeLord.cpp
  hwui/AnimatedImageDrawable.cpp
  hwui/AnimatedImageThread.cpp
  hwui/Bitmap.cpp
  hwui/BlurDrawLooper.cpp
  hwui/Canvas.cpp
  hwui/ImageDecoder.cpp
  hwui/MinikinSkia.cpp
  hwui/MinikinUtils.cpp
  hwui/PaintImpl.cpp
  hwui/Typeface.cpp
  thread/CommonPool.cpp
  utils/Blur.cpp
  utils/Color.cpp
  utils/LinearAllocator.cpp
  utils/StringUtils.cpp
  utils/StatsUtils.cpp
  utils/TypefaceUtils.cpp
  utils/VectorDrawableUtils.cpp
  AnimationContext.cpp
  Animator.cpp
  AnimatorManager.cpp
  CanvasTransform.cpp
  DamageAccumulator.cpp
  DeviceInfo.cpp
  FrameInfo.cpp
  FrameInfoVisualizer.cpp
  FrameMetricsReporter.cpp
  Gainmap.cpp
  Interpolator.cpp
  JankTracker.cpp
  Layer.cpp
  LayerUpdateQueue.cpp
  LightingInfo.cpp
  Matrix.cpp
  Mesh.cpp
  MemoryPolicy.cpp
  PathParser.cpp
  ProfileData.cpp
  Properties.cpp
  PropertyValuesAnimatorSet.cpp
  PropertyValuesHolder.cpp
  RecordingCanvas.cpp
  RenderNode.cpp
  RenderProperties.cpp
  RootRenderNode.cpp
  SkiaCanvas.cpp
  SkiaInterpolator.cpp
  Tonemapper.cpp
  TreeInfo.cpp
  VectorDrawable.cpp
  platform/host/renderthread/CacheManager.cpp
  platform/host/renderthread/HintSessionWrapper.cpp
  platform/host/renderthread/ReliableSurface.cpp
  platform/host/renderthread/RenderThread.cpp
  platform/host/ProfileDataContainer.cpp
  platform/host/Readback.cpp
  platform/host/WebViewFunctorManager.cpp
)

apex_common_sources=(
  apex/android_canvas.cpp
  apex/android_matrix.cpp
  apex/android_paint.cpp
  apex/android_region.cpp
  apex/properties.cpp
)

[[ "${#sources[@]}" == "$HWUI_SOURCE_COUNT" ]] ||
  fail_source "internal source count mismatch"
source_manifest="$({
  for source in "${sources[@]}"; do
    [[ -f "$hwui/$source" ]] || fail_source "missing module source $hwui/$source"
    printf '%s  %s\n' "$(shasum -a 256 "$hwui/$source" | awk '{print $1}')" "$source"
  done
})"
actual_manifest_sha="$(printf '%s\n' "$source_manifest" | shasum -a 256 | awk '{print $1}')"
[[ "$actual_manifest_sha" == "$HWUI_SOURCE_MANIFEST_SHA256" ]] ||
  fail_source "source manifest mismatch expected=$HWUI_SOURCE_MANIFEST_SHA256 actual=$actual_manifest_sha"
apex_manifest="$({
  for source in "${apex_common_sources[@]}"; do
    [[ -f "$hwui/$source" ]] || fail_source "missing APEX-common source $hwui/$source"
    printf '%s  %s\n' "$(shasum -a 256 "$hwui/$source" | awk '{print $1}')" "$source"
  done
})"
actual_apex_manifest_sha="$(printf '%s\n' "$apex_manifest" | shasum -a 256 | awk '{print $1}')"
[[ "${#apex_common_sources[@]}" == "$APEX_COMMON_SOURCE_COUNT" &&
   "$actual_apex_manifest_sha" == "$APEX_COMMON_SOURCE_MANIFEST_SHA256" ]] ||
  fail_source "APEX-common source manifest mismatch"

sysprop_root="${DARWIN_ART_ANDROID16_SYSPROP_ROOT:-$deps_root/system-tools-sysprop}"
sysprop_cpp="${DARWIN_ART_ANDROID16_SYSPROP_CPP:-$project_root/_build/hwui-static-deps/sysprop_cpp}"
[[ -n "$sysprop_root" && -n "$sysprop_cpp" && -x "$sysprop_cpp" ]] || {
  echo "hwui-static-foundation: pinned upstream sysprop_cpp is required" >&2
  echo "  project=$SYSPROP_PROJECT revision=$SYSPROP_REVISION" >&2
  echo "  set DARWIN_ART_ANDROID16_SYSPROP_ROOT and DARWIN_ART_ANDROID16_SYSPROP_CPP" >&2
  exit 2
}
verify_sha "$sysprop_root/Android.bp" "$SYSPROP_ANDROID_BP_SHA256"
verify_sha "$sysprop_root/sysprop.proto" "$SYSPROP_PROTO_SHA256"
verify_sha "$sysprop_root/Common.cpp" "$SYSPROP_COMMON_CPP_SHA256"
verify_sha "$sysprop_root/CodeWriter.cpp" "$SYSPROP_CODE_WRITER_CPP_SHA256"
verify_sha "$sysprop_root/CppGen.cpp" "$SYSPROP_CPP_GEN_CPP_SHA256"
verify_sha "$sysprop_root/CppMain.cpp" "$SYSPROP_CPP_MAIN_CPP_SHA256"

gui_include="${DARWIN_ART_ANDROID16_GUI_INCLUDE:-$deps_root/frameworks-native/libs/gui/include}"
binder_include="${DARWIN_ART_ANDROID16_BINDER_INCLUDE:-$deps_root/frameworks-native/libs/binder/include}"
nativedisplay_include="${DARWIN_ART_ANDROID16_NATIVEDISPLAY_INCLUDE:-$deps_root/frameworks-native/libs/nativedisplay/include}"
libhardware_include="${DARWIN_ART_ANDROID16_LIBHARDWARE_INCLUDE:-$deps_root/libhardware/include_all}"
verify_sha "$libhardware_include/hardware/hardware.h" "$LIBHARDWARE_HARDWARE_H_SHA256"

required_dirs=(
  "$gui_include"
  "$binder_include"
  "$nativedisplay_include"
  "$libhardware_include"
  "$aosp_root/frameworks/base/libs/androidfw/include"
  "$aosp_root/frameworks/native/include"
  "$aosp_root/frameworks/native/include/private"
  "$aosp_root/frameworks/native/libs/arect/include"
  "$aosp_root/frameworks/native/libs/math/include"
  "$aosp_root/frameworks/native/libs/nativebase/include"
  "$aosp_root/frameworks/native/libs/nativewindow/include"
  "$aosp_root/frameworks/native/libs/ui/include"
  "$aosp_root/frameworks/native/libs/ui/include_types"
  "$aosp_root/frameworks/native/opengl/include"
  "$aosp_root/frameworks/minikin/include"
  "$aosp_root/system/core/libcutils/include"
  "$aosp_root/system/core/libsystem/include"
  "$aosp_root/system/core/libutils/include"
  "$aosp_root/system/incremental_delivery/incfs/util/include"
  "$aosp_root/system/libbase/include"
  "$aosp_root/system/logging/liblog/include"
  "$aosp_root/external/fmtlib/include"
  "$aosp_root/external/googletest/googletest/include"
  "$aosp_root/external/harfbuzz_ng/src"
  "$aosp_root/external/icu-graphics/android_icu4c/include"
  "$aosp_root/external/icu-graphics/icu4c/source/common"
  "$aosp_root/external/skia"
  "$aosp_root/external/vulkan-headers/include"
  "$aosp_root/libnativehelper-full/include"
  "$aosp_root/libnativehelper-full/include_platform"
  "$aosp_root/libnativehelper/header_only_include"
  "$aosp_root/libnativehelper/include_jni"
)
for dir in "${required_dirs[@]}"; do
  [[ -d "$dir" ]] || fail_source "missing exported include tree $dir"
done

cxx="$(command -v clang++ || true)"
[[ -n "$cxx" ]] || fail_source "clang++ is required"
ar="$(xcrun --find ar)"
sdk_root="$(xcrun --sdk macosx --show-sdk-path)"

mkdir -p "$output_dir"
stage="$(mktemp -d "$output_dir/stage.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
cache_dir="$output_dir/objects"
generated_dir="$output_dir/generated"
mkdir -p "$cache_dir" "$generated_dir/include" "$generated_dir/source" "$generated_dir/public"
"$sysprop_cpp" \
  --header-dir "$generated_dir/include" \
  --source-dir "$generated_dir/source" \
  --include-name HWUIProperties.sysprop.h \
  --public-header-dir "$generated_dir/public" \
  "$hwui/HWUIProperties.sysprop"
verify_sha "$generated_dir/source/HWUIProperties.sysprop.cpp" \
  "$GENERATED_HWUI_PROPERTIES_CPP_SHA256"
verify_sha "$generated_dir/include/HWUIProperties.sysprop.h" \
  "$GENERATED_HWUI_PROPERTIES_H_SHA256"

flags=(
  -arch arm64 -isysroot "$sdk_root" -std=c++23 -O2 -fPIC -fno-rtti
  -fvisibility=hidden -Wall -Werror -Wextra -Wthread-safety
  -DHWUI_NULL_GPU -DNULL_GPU_MAX_TEXTURE_SIZE=4096
  '-D__INTRODUCED_IN(n)='
  '-DSK_USER_CONFIG_HEADER="include/config/SkUserConfigManual.h"'
  '-DLOG_TAG="OpenGLRenderer"' -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES
  -DATRACE_TAG=ATRACE_TAG_VIEW
  -Wno-unknown-warning-option -Wno-invalid-specialization
  -Wno-unused-parameter -Wno-unused-private-field -Wno-deprecated-declarations
  -Wno-unused-const-variable
  -Wno-unused-variable
  -Wno-sign-compare
  -Wno-inconsistent-missing-override -Wno-abstract-final-class
  -Wno-deprecated-literal-operator -Wno-missing-field-initializers
  -I"$generated_dir/include"
  -I"$hwui" -I"$hwui/platform/host"
  -I"$aosp_root/frameworks/base/libs/androidfw/include"
  -I"$aosp_root/frameworks/native/include"
  -I"$aosp_root/frameworks/native/include/private"
  -I"$aosp_root/frameworks/native/libs/arect/include"
  -I"$aosp_root/frameworks/native/libs/math/include"
  -I"$aosp_root/frameworks/native/libs/nativebase/include"
  -I"$aosp_root/frameworks/native/libs/nativewindow/include"
  -I"$aosp_root/frameworks/native/libs/ui/include"
  -I"$aosp_root/frameworks/native/libs/ui/include_types"
  -I"$aosp_root/frameworks/native/opengl/include"
  -I"$gui_include" -I"$binder_include" -I"$nativedisplay_include" -I"$libhardware_include"
  -I"$aosp_root/frameworks/minikin/include"
  -I"$aosp_root/system/core/libcutils/include"
  -I"$aosp_root/system/core/libsystem/include"
  -I"$aosp_root/system/core/libutils/include"
  -I"$aosp_root/system/incremental_delivery/incfs/util/include"
  -I"$aosp_root/system/libbase/include"
  -I"$aosp_root/system/logging/liblog/include"
  -I"$aosp_root/external/fmtlib/include"
  -I"$aosp_root/external/googletest/googletest/include"
  -I"$aosp_root/external/harfbuzz_ng/src"
  -I"$aosp_root/external/icu-graphics/android_icu4c/include"
  -I"$aosp_root/external/icu-graphics/icu4c/source/common"
  -I"$aosp_root/external/skia"
  -I"$aosp_root/external/skia/client_utils/android"
  -I"$aosp_root/external/skia/include/android"
  -I"$aosp_root/external/skia/include/codec"
  -I"$aosp_root/external/skia/include/core"
  -I"$aosp_root/external/skia/include/effects"
  -I"$aosp_root/external/skia/include/encode"
  -I"$aosp_root/external/skia/include/gpu"
  -I"$aosp_root/external/skia/include/pathops"
  -I"$aosp_root/external/skia/include/private"
  -I"$aosp_root/external/skia/include/utils"
  -I"$aosp_root/external/skia/src/core"
  -I"$aosp_root/external/vulkan-headers/include"
  -I"$aosp_root/libnativehelper-full/include"
  -I"$aosp_root/libnativehelper-full/include_platform"
  -I"$aosp_root/libnativehelper/header_only_include"
  -I"$aosp_root/libnativehelper/include_jni"
)

objects=()
compile_cached() {
  local label="$1" source="$2" object="$3"
  shift 3
  local meta="${object}.cmd"
  local source_sha
  source_sha="$(shasum -a 256 "$source" | awk '{print $1}')"
  local -a command=("$cxx" "${flags[@]}" "$@" -c "$source" -o "$object")
  local command_text
  command_text="$(printf '%q ' "${command[@]}")"
  local key
  key="$(printf '%s\n%s\n' "$source_sha" "$command_text" | shasum -a 256 | awk '{print $1}')"
  if [[ -f "$object" && -f "$meta" && "$(<"$meta")" == "$key" ]]; then
    echo "hwui-static-foundation: cache $label"
    return
  fi
  echo "hwui-static-foundation: compile $label"
  "${command[@]}"
  printf '%s\n' "$key" > "$meta"
}
for source in "${sources[@]}"; do
  object="$cache_dir/${source//\//_}.o"
  compile_cached "$source" "$hwui/$source" "$object"
  objects+=("$object")
done
generated_object="$cache_dir/HWUIProperties.sysprop.cpp.o"
compile_cached "generated HWUIProperties.sysprop.cpp" \
  "$generated_dir/source/HWUIProperties.sysprop.cpp" "$generated_object"
objects+=("$generated_object")

apex_objects=()
for source in "${apex_common_sources[@]}"; do
  object="$cache_dir/${source//\//_}.o"
  compile_cached "APEX-common $source" "$hwui/$source" "$object" \
    -I"$hwui/apex/include" -I"$hwui/jni"
  apex_objects+=("$object")
done

archive="$stage/libhwui-static-darwin.a"
"$ar" rcs "$archive" "${objects[@]}"
apex_archive="$stage/libandroid-graphics-apex-common-darwin.a"
"$ar" rcs "$apex_archive" "${apex_objects[@]}"
file "$archive" | grep -F 'current ar archive' >/dev/null
lipo -info "$archive" | grep -F 'architecture: arm64' >/dev/null
member_count="$("$ar" -t "$archive" | grep -v '^__\.SYMDEF' | wc -l | tr -d ' ')"
[[ "$member_count" == 81 ]] || {
  echo "hwui-static-foundation: archive member count expected=81 actual=$member_count" >&2
  exit 3
}
apex_member_count="$("$ar" -t "$apex_archive" | grep -v '^__\.SYMDEF' | wc -l | tr -d ' ')"
[[ "$apex_member_count" == 5 ]] || {
  echo "hwui-static-foundation: APEX-common member count expected=5 actual=$apex_member_count" >&2
  exit 3
}
definitions="$(nm -gUC "$archive")"
for symbol in \
  'android::SkiaCanvas::drawCircle(float, float, float, android::Paint const&)' \
  'android::uirenderer::renderthread::RenderThread::getInstance()' \
  'android::uirenderer::RenderNode::RenderNode('; do
  grep -F "$symbol" <<<"$definitions" >/dev/null || {
    echo "hwui-static-foundation: missing representative definition $symbol" >&2
    exit 3
  }
done

undefined_manifest="$output_dir/archive-undefined-symbols.txt"
nm -u "$archive" | awk '$1 ~ /^_/ { print $1 }' | sort -u > "$stage/archive-undefined-symbols.txt"
mv "$archive" "$output_dir/libhwui-static-darwin.a"
mv "$apex_archive" "$output_dir/libandroid-graphics-apex-common-darwin.a"
mv "$stage/archive-undefined-symbols.txt" "$undefined_manifest"
echo "hwui-static-foundation: objects=81 architecture=arm64"
echo "hwui-static-foundation: archive=$output_dir/libhwui-static-darwin.a"
echo "hwui-static-foundation: apex-common=$output_dir/libandroid-graphics-apex-common-darwin.a"
echo "hwui-static-foundation: unresolved-manifest=$undefined_manifest"
