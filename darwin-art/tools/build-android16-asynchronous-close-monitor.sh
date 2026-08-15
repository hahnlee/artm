#!/bin/bash
set -euo pipefail
export LC_ALL=C

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
lock_file="$project_root/upstream/android16-asynchronous-close-monitor.lock"
source_root="$project_root/_aosp/libcore-asynchronous-close"
build_dir="$project_root/_build/asynchronous-close-monitor"

# shellcheck disable=SC1090
source "$lock_file"

sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
fail() { echo "async-close: $*" >&2; exit 3; }

materialize() {
  local relative="$1" expected="$2"
  local destination="$source_root/$relative"
  if [[ -f "$destination" ]]; then
    [[ "$(sha256 "$destination")" == "$expected" ]] ||
      fail "checksum mismatch: $destination"
    return
  fi
  mkdir -p "$(dirname "$destination")"
  local staged
  staged="$(mktemp "${destination}.download.XXXXXX")"
  curl -fsSL \
    "https://android.googlesource.com/$LIBCORE_PROJECT/+/$LIBCORE_REVISION/$relative?format=TEXT" \
    | base64 -D > "$staged"
  [[ "$(sha256 "$staged")" == "$expected" ]] || {
    rm -f "$staged"
    fail "download checksum mismatch: $relative"
  }
  mv "$staged" "$destination"
}

materialize NativeCode.bp "$NATIVE_CODE_BP_SHA256"
materialize luni/src/main/native/Android.bp "$NATIVE_ANDROID_BP_SHA256"
materialize luni/src/main/native/Register.cpp "$REGISTER_CPP_SHA256"
materialize luni/src/main/native/AsynchronousCloseMonitor.cpp "$MONITOR_CPP_SHA256"
materialize luni/src/main/native/AsynchronousCloseMonitor.h "$MONITOR_H_SHA256"
materialize luni/src/main/native/libcore_io_AsynchronousCloseMonitor.cpp \
  "$REGISTRAR_CPP_SHA256"
materialize luni/src/main/java/libcore/io/AsynchronousCloseMonitor.java \
  "$MANAGED_CLASS_SHA256"
materialize luni/src/main/java/libcore/io/IoBridge.java "$IO_BRIDGE_SHA256"

native_bp="$source_root/luni/src/main/native/Android.bp"
native_code_bp="$source_root/NativeCode.bp"
registrar_source="$source_root/luni/src/main/native/libcore_io_AsynchronousCloseMonitor.cpp"
register_source="$source_root/luni/src/main/native/Register.cpp"
io_bridge="$source_root/luni/src/main/java/libcore/io/IoBridge.java"

stage="$(mktemp -d "${TMPDIR:-/tmp}/darwin-art-async-close.XXXXXX")"
trap 'rm -rf "$stage"' EXIT

source_manifest="$stage/libandroidio-sources.txt"
python3 - "$native_bp" > "$source_manifest" <<'PY'
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
start = text.index('name: "libandroidio_srcs"')
body_start = text.index('srcs: [', start) + len('srcs: [')
body_end = text.index('],', body_start)
for source in re.findall(r'"([^" ]+\.cpp)"', text[body_start:body_end]):
    print(source)
PY
source_count="$(wc -l < "$source_manifest" | tr -d ' ')"
source_list_sha="$(sha256 "$source_manifest")"
[[ "$source_count" == "$LIBANDROIDIO_SOURCE_COUNT" &&
   "$source_list_sha" == "$LIBANDROIDIO_SOURCE_LIST_SHA256" ]] ||
  fail "libandroidio Android.bp source identity changed count=$source_count sha=$source_list_sha"
[[ "$(<"$source_manifest")" == AsynchronousCloseMonitor.cpp ]] ||
  fail "libandroidio source selection is not module-complete"

python3 - "$native_code_bp" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
androidio = text[text.index('name: "libandroidio"'):]
androidio = androidio[:androidio.index('\n}')]
assert 'srcs: [\n        ":libandroidio_srcs",\n    ]' in androidio
assert 'shared_libs: [\n        "liblog",\n    ]' in androidio
javacore = text[text.index('name: "libjavacore"'):]
javacore = javacore[:javacore.index('\n}')]
assert '":luni_native_srcs"' in javacore
assert '"libandroidio"' in javacore
assert '"libnativehelper#impl"' in text
PY

grep -F 'REGISTER(register_libcore_io_AsynchronousCloseMonitor);' \
  "$register_source" >/dev/null || fail "upstream registrar call missing"
register_line="$(grep -n 'REGISTER(register_libcore_io_AsynchronousCloseMonitor);' \
  "$register_source" | cut -d: -f1)"
linux_line="$(grep -n 'REGISTER(register_libcore_io_Linux);' \
  "$register_source" | cut -d: -f1)"
[[ "$register_line" -lt "$linux_line" ]] || fail "upstream registration order changed"
grep -F 'AsynchronousCloseMonitor.signalBlockedThreads(oldFd);' \
  "$io_bridge" >/dev/null || fail "IoBridge signal-before-close contract missing"
method_count="$(grep -c 'NATIVE_METHOD(AsynchronousCloseMonitor,' \
  "$registrar_source")"
[[ "$method_count" == "$REGISTRAR_METHOD_COUNT" ]] ||
  fail "registrar method count=$method_count"
grep -F 'signalBlockedThreads, "(Ljava/io/FileDescriptor;)V"' \
  "$registrar_source" >/dev/null || fail "exact managed JNI signature missing"

cxx="$(xcrun --find clang++)"
libtool_bin="$(xcrun --find libtool)"
sdk_root="$(xcrun --sdk macosx --show-sdk-path)"
nativehelper="$project_root/_build/nativehelper-foundation/source/libnativehelper"
nativehelper_archive="$project_root/_build/nativehelper-foundation/libnativehelper_jvm.a"
liblog_archive="$project_root/_build/graphics-foundations/liblog-darwin.a"
for required in \
  "$project_root/compat/AsynchronousCloseMonitor.h" \
  "$project_root/compat/darwin_asynchronous_close_monitor.cc" \
  "$project_root/probes/android16_asynchronous_close_monitor_smoke.cc" \
  "$project_root/probes/android16_asynchronous_close_monitor_jni.cc" \
  "$nativehelper/include/nativehelper/JNIHelp.h" \
  "$nativehelper/include_platform/nativehelper/JNIPlatformHelp.h" \
  "$nativehelper/include_platform_header_only/nativehelper/jni_macros.h" \
  "$nativehelper/include_jni/jni.h" \
  "$nativehelper_archive" "$liblog_archive"; do
  [[ -e "$required" ]] || {
    echo "async-close: missing build dependency: $required" >&2
    exit 2
  }
done

common_flags=(
  -std=c++20 -arch arm64 -isysroot "$sdk_root" -fPIC
  -Wall -Wextra -Werror
  -I"$project_root/compat"
  -I"$nativehelper/include_jni"
  -I"$nativehelper/include"
  -I"$nativehelper/include_platform"
  -I"$nativehelper/include_platform_header_only"
  -I"$nativehelper/header_only_include"
  -I"$project_root/_aosp/system/logging/liblog/include"
)

backend_object="$stage/darwin_asynchronous_close_monitor.o"
registrar_object="$stage/libcore_io_AsynchronousCloseMonitor.o"
"$cxx" "${common_flags[@]}" \
  -c "$project_root/compat/darwin_asynchronous_close_monitor.cc" \
  -o "$backend_object"
"$cxx" "${common_flags[@]}" -c "$registrar_source" -o "$registrar_object"
for object in "$backend_object" "$registrar_object"; do
  [[ "$(file "$object")" == *"Mach-O 64-bit object arm64"* ]] ||
    fail "non-arm64 object: $object"
done

backend_archive="$stage/libandroidio-darwin.a"
registrar_archive="$stage/libcore-io-asynchronous-close-monitor-registrar-darwin.a"
"$libtool_bin" -static -o "$backend_archive" "$backend_object"
"$libtool_bin" -static -o "$registrar_archive" "$registrar_object"
for archive in "$backend_archive" "$registrar_archive"; do
  member_count="$({ ar -t "$archive" || true; } | grep -v '^__\.SYMDEF' | wc -l | tr -d ' ')"
  [[ "$member_count" == 1 && "$(lipo -archs "$archive")" == arm64 ]] ||
    fail "archive identity mismatch: $archive members=$member_count"
done

backend_definitions="$stage/backend-definitions.txt"
registrar_definitions="$stage/registrar-definitions.txt"
nm -gU "$backend_archive" | sort -u > "$backend_definitions"
nm -gU "$registrar_archive" | c++filt | sort -u > "$registrar_definitions"
for symbol in \
  _async_close_monitor_static_init \
  _async_close_monitor_signal_blocked_threads \
  _async_close_monitor_create \
  _async_close_monitor_destroy \
  _async_close_monitor_was_signalled; do
  grep -E "[[:space:]]T $symbol$" "$backend_definitions" >/dev/null ||
    fail "libandroidio definition missing: $symbol"
done
grep -F ' T register_libcore_io_AsynchronousCloseMonitor(_JNIEnv*)' \
  "$registrar_definitions" >/dev/null || fail "registrar definition missing"

smoke="$stage/asynchronous-close-monitor-smoke"
"$cxx" "${common_flags[@]}" \
  "$project_root/probes/android16_asynchronous_close_monitor_smoke.cc" \
  "$backend_object" "$liblog_archive" -o "$smoke"
smoke_output="$($smoke)"
[[ "$smoke_output" == 'async-close: two-blocked-readers=EINTR signaled=2' ]] ||
  fail "blocking smoke failed: $smoke_output"

jni_object="$stage/asynchronous-close-monitor-jni.o"
"$cxx" "${common_flags[@]}" \
  -c "$project_root/probes/android16_asynchronous_close_monitor_jni.cc" \
  -o "$jni_object"
managed_library="$stage/libasynchronous-close-monitor-managed.dylib"
"$cxx" -arch arm64 -isysroot "$sdk_root" -dynamiclib \
  "$jni_object" "$registrar_object" "$backend_object" \
  -Wl,-force_load,"$nativehelper_archive" "$liblog_archive" \
  -o "$managed_library"

java_sources="$stage/java-sources"
java_classes="$stage/java-classes"
mkdir -p "$java_sources/libcore/io" "$java_sources/dev/darwinart/probe" \
  "$java_classes"
cat > "$java_sources/libcore/io/AsynchronousCloseMonitor.java" <<'JAVA'
package libcore.io;

import java.io.FileDescriptor;

public final class AsynchronousCloseMonitor {
    private AsynchronousCloseMonitor() {}
    public static native void signalBlockedThreads(FileDescriptor fd);
}
JAVA
cat > "$java_sources/dev/darwinart/probe/AsynchronousCloseMonitorSmoke.java" <<'JAVA'
package dev.darwinart.probe;

import java.io.FileInputStream;
import libcore.io.AsynchronousCloseMonitor;

public final class AsynchronousCloseMonitorSmoke {
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("dylib required");
        System.load(args[0]);
        try (FileInputStream input = new FileInputStream("/dev/null")) {
            AsynchronousCloseMonitor.signalBlockedThreads(input.getFD());
        }
        System.out.println("managed-async-close: registrar=pass signature=FileDescriptor->void");
    }
}
JAVA
javac --release 17 -encoding UTF-8 -d "$java_classes" \
  "$java_sources/libcore/io/AsynchronousCloseMonitor.java" \
  "$java_sources/dev/darwinart/probe/AsynchronousCloseMonitorSmoke.java"
managed_output="$(java -cp "$java_classes" \
  dev.darwinart.probe.AsynchronousCloseMonitorSmoke "$managed_library")"
[[ "$managed_output" == \
   'managed-async-close: registrar=pass signature=FileDescriptor->void' ]] ||
  fail "managed registrar smoke failed: $managed_output"

mkdir -p "$build_dir"
cp "$backend_archive" "$build_dir/libandroidio-darwin.a"
cp "$registrar_archive" \
  "$build_dir/libcore-io-asynchronous-close-monitor-registrar-darwin.a"
cp "$smoke" "$build_dir/asynchronous-close-monitor-smoke"

echo "async-close: libandroidio=1 registrar=1 signal=SIGUSR2 blocked-readers=2/EINTR managed=pass"
