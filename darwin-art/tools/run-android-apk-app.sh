#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
installed_record=""
if [[ "${1:-}" == "--record" ]]; then
  installed_record="${2:-}"
  seconds="${3:-86400}"
  [[ -f "$installed_record" ]] || {
    echo "installed launch record does not exist: $installed_record" >&2
    exit 66
  }
  [[ "$(sed -n '1p' "$installed_record")" == "darwin-art-launch-v1" ]] || {
    echo "installed launch record version is unsupported" >&2
    exit 65
  }
  apk="$(sed -n 's/^apk=//p' "$installed_record")"
  app_dex="$(sed -n 's/^dex=//p' "$installed_record")"
  metadata="$(sed -n 's/^metadata=//p' "$installed_record")"
else
  apk="${1:-}"
  seconds="${2:-86400}"
fi
[[ -n "$apk" ]] || {
  echo "usage: $0 APK [VISIBLE_SECONDS] | --record RECORD [VISIBLE_SECONDS]" >&2
  exit 64
}
source_apk="$(cd "$(dirname "$apk")" && pwd)/$(basename "$apk")"
[[ -f "$source_apk" ]] || {
  echo "APK does not exist: $source_apk" >&2
  exit 66
}
[[ "$seconds" =~ ^([0-9]+)(\.[0-9]+)?$ ]] || {
  echo "VISIBLE_SECONDS must be a non-negative number" >&2
  exit 64
}

if [[ -z "$installed_record" ]]; then
  app_dex="$source_apk"
  external_dex="${source_apk%.apk}.dex"
  if ! unzip -Z1 "$source_apk" | grep -Fx 'classes.dex' >/dev/null; then
    [[ -f "$external_dex" ]] || {
      echo "preoptimized APK requires its deoptimized DEX sidecar: $external_dex" >&2
      exit 69
    }
    app_dex="$external_dex"
  fi
  if [[ "$app_dex" == "$source_apk" ]]; then
    metadata="$(cargo run -q --release \
      --manifest-path "$root/tools/android-apk-app-runtime/Cargo.toml" -- "$source_apk")"
  else
    metadata="$(cargo run -q --release \
      --manifest-path "$root/tools/android-apk-app-runtime/Cargo.toml" -- "$source_apk" "$app_dex")"
  fi
fi
package="$(sed -n 's/^apk-app-runtime: package=\([^ ]*\) .*/\1/p' <<<"$metadata")"
application="$(sed -n 's/^apk-app-runtime: .* application=\([^ ]*\) .*/\1/p' <<<"$metadata")"
activity="$(sed -n 's/^apk-app-runtime: .* activity=\([^ ]*\) .*/\1/p' <<<"$metadata")"
launch_component="$(sed -n 's/^apk-app-runtime: .* launch_component=\([^ ]*\) .*/\1/p' <<<"$metadata")"
descriptor="$(sed -n 's/^apk-app-runtime: .* descriptor=\([^ ]*\) .*/\1/p' <<<"$metadata")"
activities="$(sed -n 's/^apk-app-runtime: .* activities=\([^ ]*\) .*/\1/p' <<<"$metadata")"
activity_aliases="$(sed -n 's/^apk-app-runtime: .* activity_aliases=\([^ ]*\) .*/\1/p' <<<"$metadata")"
services="$(sed -n 's/^apk-app-runtime: .* services=\([^ ]*\) .*/\1/p' <<<"$metadata")"
application_metadata="$(sed -n 's/^apk-app-runtime: .* application_metadata=\([^ ]*\) .*/\1/p' <<<"$metadata")"
version_code="$(sed -n 's/^apk-app-runtime: .* version_code=\([^ ]*\) .*/\1/p' <<<"$metadata")"
version_name="$(sed -n 's/^apk-app-runtime: .* version_name=\([^ ]*\) .*/\1/p' <<<"$metadata")"
theme="$(sed -n 's/^apk-app-runtime: .* theme=\([^ ]*\) .*/\1/p' <<<"$metadata")"
target_sdk="$(sed -n 's/^apk-app-runtime: .* target_sdk=\([^ ]*\) .*/\1/p' <<<"$metadata")"
label="$(sed -n 's/^apk-app-runtime: .* label=\(.*\) label_res=.*/\1/p' <<<"$metadata")"
label_res="$(sed -n 's/^apk-app-runtime: .* label_res=\([^ ]*\) .*/\1/p' <<<"$metadata")"
icon="$(sed -n 's/^apk-app-runtime: .* icon=\([^ ]*\) .*/\1/p' <<<"$metadata")"
native_count="$(sed -n 's/^apk-app-runtime: .* native=\([^ ]*\) .*/\1/p' <<<"$metadata")"
native_root="$(sed -n 's/^apk-app-runtime: .* native_root=\([^ ]*\)$/\1/p' <<<"$metadata")"
[[ -n "$package" && -n "$application" && -n "$activity" && -n "$launch_component" && -n "$descriptor" && -n "$activities" && -n "$activity_aliases" && -n "$services" && -n "$application_metadata" && -n "$version_code" && -n "$theme" && -n "$target_sdk" && -n "$label" && -n "$label_res" && -n "$icon" && -n "$native_count" && -n "$native_root" ]] || {
  echo "could not decode inspected APK metadata" >&2
  exit 65
}
if [[ -n "${DARWIN_ART_APK_ACTIVITY_OVERRIDE:-}" ]]; then
  requested_activity="$DARWIN_ART_APK_ACTIVITY_OVERRIDE"
  requested_entry="$(tr ',' '\n' <<<"$activities" | \
    sed -n "s#^${requested_activity}=##p" | head -1)"
  [[ -n "$requested_entry" ]] || {
    echo "requested Activity is not declared by the APK: $requested_activity" >&2
    exit 65
  }
  activity="$requested_activity"
  launch_component="$requested_activity"
  descriptor="L$(tr '.' '/' <<<"$activity");"
  theme="$requested_entry"
fi

runtime_abi="darwin-art-darwin-native-v1"
profile_mount=""
if [[ -z "${DARWIN_ART_APP_DATA_ROOT:-}" ]]; then
  if [[ -z "$installed_record" ]]; then
    cargo build -q --release -p darwin-art-profile --bins
  elif [[ ! -x "$root/target/release/darwin-artctl" ]]; then
    echo "installed run requires a prebuilt darwin-artctl; run cargo xtask build" >&2
    exit 69
  fi
  profile_ctl="$root/target/release/darwin-artctl"
  profile_mount="$("$profile_ctl" ensure)"
  export DARWIN_ART_PROFILE_CTL="$profile_ctl"
  export DARWIN_ART_PROFILE_SOCKET
  DARWIN_ART_PROFILE_SOCKET="$("$profile_ctl" socket)"
  export DARWIN_ART_SYSTEM_SERVER_SOCKET="$profile_mount/run/system-server-lite.sock"
fi
if [[ -n "${DARWIN_ART_APK_INSTALL_ROOT:-}" ]]; then
  install_root="$DARWIN_ART_APK_INSTALL_ROOT"
elif [[ -n "$profile_mount" ]]; then
  install_root="$profile_mount/packages"
else
  install_root="$root/_build/installed-apps"
fi
native_cache_root="${DARWIN_ART_NATIVE_CACHE_ROOT:-$root/_build/native-artifact-cache}"
native_converter="${DARWIN_ART_NATIVE_CONVERTER:-none}"
installer="$root/target/release/darwin-art-apk-install"
native_resolver="$root/target/release/darwin-art-native-resolve"
if [[ -z "$installed_record" ]]; then
  cargo build -q --release -p darwin-art-apk-install
  cargo build -q --release -p darwin-art-native-artifact --bin darwin-art-native-resolve
  extractor="none"
  if [[ "$native_count" != "0" ]]; then
    cargo build -q --release \
      --manifest-path "$root/tools/android-apk-native-extract/Cargo.toml"
    extractor="$root/target/release/android-apk-native-extract"
  fi
  install_output="$("$installer" "$source_apk" "$install_root" "$package" \
    "$version_code" "$native_root" "$extractor" "$runtime_abi" \
    "$native_cache_root" "$native_converter")"
  apk_sha256="$(sed -n 's/^apk-install: .* apk_sha256=\([^ ]*\) .*/\1/p' \
    <<<"$install_output")"
elif [[ -n "$installed_record" ]]; then
  apk_sha256="$(sed -n 's/^sha256=//p' "$installed_record")"
  install_output="apk-install: cached package=$package apk_sha256=$apk_sha256"
fi
[[ "$apk_sha256" =~ ^[0-9a-f]{64}$ ]] || {
  echo "could not decode installed APK identity" >&2
  exit 65
}
if [[ -z "$installed_record" ]]; then
  installed_directory="$install_root/$package/$version_code/$apk_sha256"
  apk="$installed_directory/base.apk"
else
  installed_directory="$(dirname "$apk")"
fi
[[ -f "$apk" ]] || {
  echo "installed APK is missing: $apk" >&2
  exit 69
}
if [[ -z "$installed_record" && "$app_dex" == "$source_apk" ]]; then
  app_dex="$apk"
fi

if [[ -z "$installed_record" && -n "$profile_mount" ]]; then
  if [[ "$app_dex" != "$apk" ]]; then
    code_directory="$profile_mount/system/package-code/$package/$apk_sha256"
    mkdir -p "$code_directory"
    persistent_dex="$code_directory/classes.dex"
    if [[ ! -f "$persistent_dex" ]]; then
      dex_stage="$code_directory/.classes.dex.$$.stage"
      cp "$app_dex" "$dex_stage"
      chmod 0400 "$dex_stage"
      mv "$dex_stage" "$persistent_dex"
    fi
    app_dex="$persistent_dex"
  fi
  record_stage="$(mktemp "$profile_mount/run/launch-record.XXXXXX")"
  {
    printf 'darwin-art-launch-v1\n'
    printf 'apk=%s\n' "$apk"
    printf 'dex=%s\n' "$app_dex"
    printf 'sha256=%s\n' "$apk_sha256"
    printf 'metadata=%s\n' "$metadata"
  } >"$record_stage"
  "$profile_ctl" register "$package" "$record_stage"
  rm -f "$record_stage"
  if [[ "${DARWIN_ART_INSTALL_ONLY:-0}" == "1" ]]; then
    host="$root/target/debug/darwin-art-host"
    [[ -x "$host" ]] || {
      echo "darwin-art host is missing; run cargo xtask build before installing" >&2
      exit 69
    }
    codesign --force --sign - --options runtime \
      --entitlements "$root/config/darwin-art-host.entitlements" "$host" >/dev/null
    echo "$metadata"
    echo "$install_output"
    echo "darwin-art: installed package=$package"
    exit 0
  fi
else
  for runtime_binary in "$native_resolver"; do
    [[ -x "$runtime_binary" ]] || {
      echo "installed run requires a prebuilt runtime; run cargo xtask build" >&2
      exit 69
    }
  done
fi

host="$root/target/debug/darwin-art-host"
[[ -x "$host" ]] || {
  echo "darwin-art host is missing: $host" >&2
  exit 69
}
# Installation signs the shared host once. Re-signing here would atomically
# replace the executable underneath concurrent launches. Legacy direct-APK
# runs still sign because they do not pass through the install command.
if [[ -z "$installed_record" ]]; then
  codesign --force --sign - --options runtime \
    --entitlements "$root/config/darwin-art-host.entitlements" "$host" >/dev/null
else
  codesign --verify --strict "$host"
fi
runtime="$root/_build/runtime-graphics-link-probe/libdarwin_art_runtime_graphics.dylib"
core_oj="${DARWIN_ART_CORE_OJ_JAR:-$root/_prebuilt/android-16/bootclasspath/core-oj.jar}"
if [[ -z "${DARWIN_ART_CORE_OJ_JAR:-}" && \
      -f "$root/_build/android16-core-oj-compat/core-oj-compat.jar" ]]; then
  core_oj="$root/_build/android16-core-oj-compat/core-oj-compat.jar"
fi
core_libart="$root/_prebuilt/android-16/bootclasspath/core-libart.jar"
framework="${DARWIN_ART_FRAMEWORK_JAR:-$root/_prebuilt/android-16/bootclasspath/framework.jar}"
if [[ -z "${DARWIN_ART_FRAMEWORK_JAR:-}" && \
      -f "$root/_build/android16-framework-compat/framework-compat.jar" ]]; then
  # The detached host has no DeviceConfig service manager.  Use the merged
  # framework boot image whose no-service DeviceConfig seam preserves AOSP
  # default-valued feature flags during widget construction.
  framework="$root/_build/android16-framework-compat/framework-compat.jar"
fi
framework_location="$root/_prebuilt/android-16/bootclasspath/framework-location.jar"
core_icu="$root/_build/bootclasspath/core-icu4j-api36.jar"
conscrypt="$root/_build/android16-ps16k-r07/extracted/conscrypt/javalib/conscrypt.jar"
conscrypt_native="$root/_build/android16-ps16k-r07/extracted/conscrypt/lib64"
framework_bluetooth="$root/_build/android16-ps16k-r07/extracted/bt/javalib/framework-bluetooth.jar"
framework_mediaprovider="$root/_build/android16-ps16k-r07/extracted/mediaprovider/javalib/framework-mediaprovider.jar"
framework_permission="$root/_build/android16-ps16k-r07/extracted/permission/javalib/framework-permission.jar"
framework_permission_s="$root/_build/android16-ps16k-r07/extracted/permission/javalib/framework-permission-s.jar"
# Match Android 16's boot-class-path ordering: framework-location follows the
# core framework (and framework-graphics, once split out here) before APEX
# framework modules.  The host ABI accepts the remaining colon-separated
# components through its boot-tail field.
boot_tail="$framework_location:$conscrypt:$framework_bluetooth:$framework_mediaprovider:$framework_permission:$framework_permission_s:$core_icu"
support_dex="$root/_build/button-dex/dex/classes.dex"
export DARWIN_ART_RUNTIME_HOST_FILES="$core_oj:$core_libart:$framework:$boot_tail:$support_dex:$app_dex"
fonts_xml="$root/probes/button/fonts.xml"
roboto="$root/_aosp/external/skia/resources/fonts/Roboto-Regular.ttf"
framework_res="$root/_prebuilt/android-16/resources/framework-res.apk"
if [[ ! -f "$support_dex" ]]; then
  if [[ -n "$installed_record" ]]; then
    echo "installed run requires prebuilt support DEX; run cargo xtask build" >&2
    exit 69
  fi
  cargo run -q -p art-bootstrap -- build-button-dex >/dev/null
fi
for input in "$host" "$runtime" "$core_oj" "$core_libart" "$framework" "$framework_location" "$core_icu" "$conscrypt" "$framework_bluetooth" "$framework_mediaprovider" "$framework_permission" "$framework_permission_s" "$support_dex" "$fonts_xml" "$roboto" "$framework_res"; do
  [[ -f "$input" ]] || {
    echo "runtime input is missing: $input" >&2
    echo "run the bootstrap/graphics build gates first" >&2
    exit 69
  }
done

# The Android font bootstrap uses the guest filesystem facade. Give arbitrary
# no-native APKs the same immutable, minimal system root as the in-tree gate;
# pointing the process at the host filesystem would bypass the guest path
# policy and make results depend on the developer machine.
if [[ -n "$profile_mount" ]]; then
  system_root="$(mktemp -d "$profile_mount/run/app.XXXXXX")"
else
  system_root="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-apk-system-root.XXXXXX")"
fi
icon_file=""
cleanup_system_root() {
  chmod -R u+w "$system_root" 2>/dev/null || true
  rm -rf "$system_root"
  [[ -z "$icon_file" ]] || rm -f "$icon_file"
}
trap cleanup_system_root EXIT
mkdir -p "$system_root/system/etc" "$system_root/system/fonts" \
  "$system_root/system/framework" "$system_root/system/lib64"
cp "$fonts_xml" "$system_root/system/etc/fonts.xml"
cp "$roboto" "$system_root/system/fonts/Roboto-Regular.ttf"
cp "$framework_res" "$system_root/system/framework/framework-res.apk"
for library in libc++.so libcrypto.so libjavacrypto.so libssl.so; do
  cp "$conscrypt_native/$library" "$system_root/system/lib64/$library"
done
chmod 0400 "$system_root/system/etc/fonts.xml" \
  "$system_root/system/fonts/Roboto-Regular.ttf" \
  "$system_root/system/framework/framework-res.apk" \
  "$system_root/system/lib64/"*.so
chmod 0700 "$system_root"
chmod 0500 "$system_root/system" "$system_root/system/etc" \
  "$system_root/system/fonts" "$system_root/system/framework"
chmod 0500 "$system_root/system/lib64"

icu_runtime="$root/_build/icu-runtime-adapters/runtime"
export ANDROID_I18N_ROOT="$icu_runtime/i18n"
export ANDROID_DATA="$icu_runtime/data"
export ANDROID_TZDATA_ROOT="$icu_runtime/tzdata"
export DARWIN_ART_APK_APP_PACKAGE="$package"
export DARWIN_ART_APK_APP_APPLICATION="$application"
export DARWIN_ART_APK_APP_ACTIVITY="$activity"
export DARWIN_ART_APK_APP_LAUNCH_COMPONENT="$launch_component"
export DARWIN_ART_APK_APP_DESCRIPTOR="$descriptor"
export DARWIN_ART_APK_APP_ACTIVITIES="$activities"
export DARWIN_ART_APK_APP_ACTIVITY_ALIASES="$activity_aliases"
export DARWIN_ART_APK_APP_SERVICES="$services"
export DARWIN_ART_APK_APP_METADATA="$application_metadata"
export DARWIN_ART_APK_APP_VERSION_CODE="$version_code"
export DARWIN_ART_APK_APP_VERSION_NAME="$version_name"
export DARWIN_ART_APK_APP_THEME="$theme"
export DARWIN_ART_APK_APP_TARGET_SDK="$target_sdk"
export DARWIN_ART_APK_APP_APK_SHA256="$apk_sha256"
export DARWIN_ART_NATIVE_RUNTIME_ABI="$runtime_abi"
export DARWIN_ART_APK_APP_LABEL="$label"
export DARWIN_ART_APK_APP_LABEL_RES="$label_res"
if [[ -n "${DARWIN_ART_APP_DATA_ROOT:-}" ]]; then
  app_data_root="$DARWIN_ART_APP_DATA_ROOT"
else
  app_data_root="$profile_mount/data/apps"
fi
app_data_dir="$app_data_root/$package"
mkdir -p "$app_data_dir"
private_data_root="$app_data_dir/private-data"
mkdir -p "$private_data_root"
chmod 0700 "$private_data_root"
export DARWIN_ART_ANDROID_PRIVATE_DATA_ROOT="$private_data_root"
export DARWIN_ART_APK_APP_DATA_DIR="$app_data_dir"
export DARWIN_ART_APK_APP_DATA_GUEST_DIR="/data/user/0/$package"

# ContextImpl creates these package-private directories before app code runs.
# The detached host exposes the same writable subtree inside the sealed guest
# root; Java and native code therefore agree on the Android /data path.
chmod 0700 "$system_root"
guest_app_data="$private_data_root/user/0/$package"
mkdir -p "$guest_app_data/files" "$guest_app_data/cache" \
  "$guest_app_data/code_cache" "$guest_app_data/no_backup" \
  "$guest_app_data/databases" "$guest_app_data/shared_prefs"
chmod 0500 "$private_data_root/user" "$private_data_root/user/0"
chmod 0700 "$guest_app_data" "$guest_app_data/files" \
  "$guest_app_data/cache" "$guest_app_data/code_cache" \
  "$guest_app_data/no_backup" "$guest_app_data/databases" \
  "$guest_app_data/shared_prefs"

# This runtime currently exposes GLES through ANGLE/Metal but no Android
# Vulkan/Dawn device. Chromium otherwise enables Graphite and Android's
# cross-thread display compositor from the SDK level alone. That combination
# asks SharedImage for cross-thread software-decoded I420 storage, which the
# truthful GLES backend cannot share. Select Chromium's supported GLES
# fallback until the compatibility layer grows multiplanar Vulkan images.
if [[ "$package" == "org.chromium.chrome" &&
      "${DARWIN_ART_CHROMIUM_GPU_COMPATIBILITY:-1}" != "0" ]]; then
  export DARWIN_ART_APP_COMMAND_LINE_FILE="${DARWIN_ART_APP_COMMAND_LINE_FILE:-chrome-command-line}"
  chromium_command_line="${DARWIN_ART_APP_COMMAND_LINE:-}"
  chromium_disabled_features="SkiaGraphite,EnableDrDc"
  if [[ "$chromium_command_line" =~ (^|[[:space:]])--disable-features=([^[:space:]]*) ]]; then
    existing_disabled_features="${BASH_REMATCH[2]}"
    for feature in SkiaGraphite EnableDrDc; do
      if [[ ",$existing_disabled_features," != *",$feature,"* ]]; then
        existing_disabled_features="${existing_disabled_features:+$existing_disabled_features,}$feature"
      fi
    done
    old_switch="--disable-features=${BASH_REMATCH[2]}"
    chromium_command_line="${chromium_command_line/"$old_switch"/"--disable-features=$existing_disabled_features"}"
  else
    chromium_command_line="${chromium_command_line:+$chromium_command_line }--disable-features=$chromium_disabled_features"
  fi
  export DARWIN_ART_APP_COMMAND_LINE="$chromium_command_line"
fi
if [[ -n "${DARWIN_ART_APP_COMMAND_LINE_FILE:-}" ]]; then
  [[ "$DARWIN_ART_APP_COMMAND_LINE_FILE" == "$(basename "$DARWIN_ART_APP_COMMAND_LINE_FILE")" ]] || {
    echo "app command-line filename must be a basename" >&2
    exit 64
  }
  [[ -n "${DARWIN_ART_APP_COMMAND_LINE:-}" ]] || {
    echo "DARWIN_ART_APP_COMMAND_LINE_FILE requires DARWIN_ART_APP_COMMAND_LINE" >&2
    exit 64
  }
  command_line_dir="$private_data_root/local"
  command_line_file="$command_line_dir/$DARWIN_ART_APP_COMMAND_LINE_FILE"
  command_line_debug_dir="$command_line_dir/tmp"
  command_line_debug_file="$command_line_debug_dir/$DARWIN_ART_APP_COMMAND_LINE_FILE"
  mkdir -p "$command_line_dir"
  chmod 0700 "$command_line_dir"
  mkdir -p "$command_line_debug_dir"
  chmod 0700 "$command_line_debug_dir"
  command_line_payload="$(tr '\n' ' ' <<<"$DARWIN_ART_APP_COMMAND_LINE")"
  [[ ! -e "$command_line_file" ]] || chmod 0600 "$command_line_file"
  [[ ! -e "$command_line_debug_file" ]] || chmod 0600 "$command_line_debug_file"
  printf '_ %s\n' "$command_line_payload" >"$command_line_file"
  printf '_ %s\n' "$command_line_payload" >"$command_line_debug_file"
  chmod 0400 "$command_line_file"
  chmod 0400 "$command_line_debug_file"
  chmod 0500 "$command_line_debug_dir"
  chmod 0500 "$command_line_dir"
fi
chmod 0500 "$private_data_root"
chmod 0500 "$system_root"

# Publish only this application's authorized external-storage directory inside
# the sealed guest root. Native Android libraries must see Android paths rather
# than arbitrary Darwin paths; the filesystem facade keeps the host authority
# rooted at this temporary capability tree.
external_storage_dir="$app_data_dir/external"
guest_external_root="$system_root/storage/emulated/0"
chmod 0700 "$system_root"
guest_external_app="$guest_external_root/Android/data/$package/files"
mkdir -p "$external_storage_dir" "$guest_external_app"
if [[ -n "$(find "$external_storage_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
  cp -R "$external_storage_dir/." "$guest_external_root/"
fi
chmod -R u=rX,go= "$system_root/storage"
chmod 0500 "$system_root"
export DARWIN_ART_APK_APP_EXTERNAL_DIR="/storage/emulated/0/Android/data/$package/files"
if [[ "$icon" != "none" ]]; then
  icon_file="$(mktemp "${TMPDIR:-/tmp}/darwin-art-apk-icon.XXXXXX")"
  unzip -p "$apk" "$icon" >"$icon_file"
  chmod 0400 "$icon_file"
  export DARWIN_ART_APK_APP_ICON="$icon_file"
else
  unset DARWIN_ART_APK_APP_ICON
fi
export DARWIN_ART_APK_APP_SUPPORT_DEX="$support_dex"
export DARWIN_ART_APK_APP_RESOURCE_APK="$apk"
export DARWIN_ART_FRAMEWORK_RES_APK="$framework_res"
export DARWIN_ART_TEST_FONTS_XML="/system/etc/fonts.xml"
export DARWIN_ART_TEST_FONT="/system/fonts/Roboto-Regular.ttf"
export DARWIN_ART_ANDROID_FILESYSTEM_ROOT="$system_root"
export DARWIN_ART_ANDROID_SYSTEM_ROOT="$system_root/system"
export DARWIN_ART_ANDROID_SYSTEM_NATIVE_DIR="$system_root/system/lib64"
export DARWIN_ART_WINDOW_SCALE=2

# A project-built ANGLE exposes Metal textures as EGLImages, which is required
# for Android AHardwareBuffer storage identity. Prefer it over the older ANGLE
# bundled with Android Studio; an explicit environment override remains first.
if [[ -z "${DARWIN_ART_ANGLE_DIRECTORY:-}" ]]; then
  angle_candidate="$root/_build/angle-source/out/DarwinArtRelease"
  if [[ -f "$angle_candidate/libEGL.dylib" &&
        -f "$angle_candidate/libGLESv2.dylib" ]]; then
    export DARWIN_ART_ANGLE_DIRECTORY="$angle_candidate"
  fi
fi
if [[ -z "${DARWIN_ART_ANGLE_DIRECTORY:-}" && -n "${ANDROID_HOME:-}" ]]; then
  angle_candidate="$ANDROID_HOME/emulator/lib64/gles_angle"
  if [[ -f "$angle_candidate/libEGL.dylib" &&
        -f "$angle_candidate/libGLESv2.dylib" ]]; then
    export DARWIN_ART_ANGLE_DIRECTORY="$angle_candidate"
  fi
fi

if [[ "$native_count" != "0" ]]; then
  unwind_provider="$root/_build/android-unwind-provider/libdarwin_art_android_unwind.so"
  if [[ -n "$installed_record" && ! -f "$unwind_provider" ]]; then
    echo "installed native run requires prebuilt Android unwind provider" >&2
    exit 69
  elif [[ -z "$installed_record" ]]; then
    "$root/tools/build-android-unwind-provider.sh" "$unwind_provider" >/dev/null
  fi
  export DARWIN_ART_ANDROID_UNWIND_PROVIDER="$unwind_provider"
  [[ "$native_root" != "none" ]] || {
    echo "APK native metadata did not select an arm64 root library" >&2
    exit 65
  }
  native_directory="$installed_directory/android-elf/arm64-v8a"
  [[ -f "$native_directory/$native_root" ]] || {
    echo "installed APK native root is missing" >&2
    exit 69
  }
  export DARWIN_ART_APK_APP_NATIVE_PATH="$native_directory/$native_root"
  export DARWIN_ART_APK_APP_NATIVE_DIR="$native_directory"
  darwin_directory="$native_cache_root/$apk_sha256/$runtime_abi"
  native_resolution="$("$native_resolver" "$apk_sha256" "$runtime_abi" \
    "$native_directory" "$darwin_directory")"
  native_backend="$(sed -n 's/^native-resolve: PASS backend=\([^ ]*\) .*/\1/p' \
    <<<"$native_resolution")"
  case "$native_backend" in
    darwin)
      export DARWIN_ART_APK_NATIVE_BACKEND=darwin
      export DARWIN_ART_APK_DARWIN_DIRECTORY="$darwin_directory"
      ;;
    elf)
      export DARWIN_ART_APK_NATIVE_BACKEND=elf
      unset DARWIN_ART_APK_DARWIN_DIRECTORY
      ;;
    *)
      echo "native artifact resolver returned an invalid backend" >&2
      exit 65
      ;;
  esac
  export DARWIN_ART_APK_MANAGED_NATIVE_LOAD="${DARWIN_ART_APK_MANAGED_NATIVE_LOAD:-1}"
else
  unset DARWIN_ART_ANDROID_UNWIND_PROVIDER
  unset DARWIN_ART_APK_APP_NATIVE_PATH
  unset DARWIN_ART_APK_APP_NATIVE_DIR
  unset DARWIN_ART_APK_MANAGED_NATIVE_LOAD
  unset DARWIN_ART_APK_NATIVE_BACKEND
  unset DARWIN_ART_APK_DARWIN_DIRECTORY
fi

echo "$metadata"
echo "$install_output"
[[ "$native_count" == "0" ]] || echo "$native_resolution"
if [[ "${DARWIN_ART_LLDB:-0}" == "1" ]]; then
  exec lldb --batch -o run -k bt -k 'register read' -- "$host" --window-seconds "$seconds" \
    "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex"
fi
if [[ "${DARWIN_ART_LLDB:-0}" == "exit" ]]; then
  exec lldb --batch \
    -o 'breakpoint set -n exit' -o 'breakpoint set -n _exit' \
    -o 'breakpoint set -n pthread_exit' \
    -o 'breakpoint set -n darwin_art_bionic_exit' \
    -o 'breakpoint set -n darwin_art_bionic__exit' \
    -o run -k 'thread backtrace all -c 40' -k 'register read' -- "$host" --window-seconds "$seconds" \
    "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex"
fi
if [[ "${DARWIN_ART_LLDB:-0}" == "dex" ]]; then
  exec lldb --batch \
    -o 'breakpoint set -n _ZN3artL25DexFile_defineClassNativeEP7_JNIEnvP7_jclassP8_jstringP8_jobjectS7_S7_' \
    -o run -o 'register read x0 x1 x2 x3 x4 x5' \
    -o 'thread backtrace -c 30' -- "$host" --window-seconds "$seconds" \
    "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex"
fi
if [[ "${DARWIN_ART_LLDB:-0}" == "syscall-240" ]]; then
  exec lldb --batch \
    -o 'breakpoint set -n darwin_art_bionic_syscall_captured -c "*(unsigned long long*)$x0 == 240"' \
    -o run -o 'memory read -fx -s8 -c6 $x0' \
    -o 'thread backtrace -c 20' -- "$host" --window-seconds "$seconds" \
    "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex"
fi
if [[ "${DARWIN_ART_LLDB:-0}" == "fs-stat" ]]; then
  exec lldb --batch \
    -o 'breakpoint set -n darwin_art_libcore_stat -c "(int)strncmp((char*)$x0, \"/data/local\", 11) == 0"' \
    -o run -o 'memory read -s1 -c128 $x0' \
    -o 'thread backtrace -c 24' -- "$host" --window-seconds "$seconds" \
    "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex"
fi
host_command=("$host" --window-seconds "$seconds" \
  "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$app_dex")
if [[ -n "$profile_mount" ]]; then
  system_server_pid="$("$profile_ctl" ps | awk '$2 == "android.system" { print $1; exit }')"
  if [[ -z "$system_server_pid" ]] || ! kill -0 "$system_server_pid" 2>/dev/null; then
    system_server_log="${profile_mount%/mnt}/darwin-artd.log"
    system_server_root="$profile_mount/system-server-root"
    system_server_private="$profile_mount/data/apps/android.system/private-data"
    if [[ ! -d "$system_server_root/system" ]]; then
      mkdir -p "$system_server_root" "$system_server_private/user/0/android"
      cp -R "$system_root/system" "$system_server_root/system"
      mkdir -p "$system_server_root/storage/emulated/0"
      chmod -R u=rX,go= "$system_server_root/system" "$system_server_root/storage"
    else
      mkdir -p "$system_server_private/user/0/android"
    fi
    chmod 0700 "$system_server_private" "$system_server_private/user/0/android"
    chmod 0500 "$system_server_root" "$system_server_private"
    system_server_command=("$host" --window-seconds 0 \
      "$runtime" "$core_oj" "$core_libart" "$framework" "$boot_tail" "$support_dex")
    system_server_launcher_pid="$( \
      DARWIN_ART_SYSTEM_SERVER_MODE=1 \
      DARWIN_ART_APK_APP_PACKAGE=android \
      DARWIN_ART_APK_APP_APPLICATION=android.app.Application \
      DARWIN_ART_APK_APP_ACTIVITY=dev.darwinart.probe.ProbeActivity \
      DARWIN_ART_APK_APP_LAUNCH_COMPONENT=none \
      DARWIN_ART_APK_APP_DESCRIPTOR=Ldev/darwinart/probe/ProbeActivity\; \
      DARWIN_ART_APK_APP_ACTIVITIES=none \
      DARWIN_ART_APK_APP_ACTIVITY_ALIASES=none \
      DARWIN_ART_APK_APP_SERVICES=none \
      DARWIN_ART_APK_APP_METADATA=none \
      DARWIN_ART_APK_APP_VERSION_CODE=1 \
      DARWIN_ART_APK_APP_VERSION_NAME=1 \
      DARWIN_ART_APK_APP_THEME=0 \
      DARWIN_ART_APK_APP_TARGET_SDK=36 \
      DARWIN_ART_APK_APP_LABEL=Android \
      DARWIN_ART_APK_APP_LABEL_RES=0 \
      DARWIN_ART_APK_APP_RESOURCE_APK="$framework_res" \
      DARWIN_ART_ANDROID_PRIVATE_DATA_ROOT="$system_server_private" \
      DARWIN_ART_APK_APP_DATA_DIR="${system_server_private%/private-data}" \
      DARWIN_ART_APK_APP_DATA_GUEST_DIR=/data/user/0/android \
      DARWIN_ART_APK_APP_EXTERNAL_DIR=/storage/emulated/0 \
      DARWIN_ART_ANDROID_FILESYSTEM_ROOT="$system_server_root" \
      DARWIN_ART_ANDROID_SYSTEM_ROOT="$system_server_root/system" \
      DARWIN_ART_ANDROID_SYSTEM_NATIVE_DIR="$system_server_root/system/lib64" \
      DARWIN_ART_RUNTIME_HOST_FILES="$core_oj:$core_libart:$framework:$boot_tail:$support_dex" \
      "$profile_ctl" daemonize android.system "${system_server_command[@]}" \
    )"
    for _ in {1..100}; do
      [[ -S "$DARWIN_ART_SYSTEM_SERVER_SOCKET" ]] && break
      kill -0 "$system_server_launcher_pid" 2>/dev/null || break
      sleep 0.05
    done
    [[ -S "$DARWIN_ART_SYSTEM_SERVER_SOCKET" ]] || {
      echo "system_server-lite did not publish its package Binder" >&2
      tail -40 "$system_server_log" >&2 || true
      exit 70
    }
  fi
  exec "$profile_ctl" exec "$package" "${host_command[@]}"
fi
exec "${host_command[@]}"
