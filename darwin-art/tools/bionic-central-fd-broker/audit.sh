#!/bin/bash
set -euo pipefail
export LC_ALL=C

dir="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$dir/../.." && pwd)"
fail() { echo "bionic-central-fd-broker: FAIL $*" >&2; exit 2; }

tmp="$(mktemp -d "${TMPDIR:-/tmp}/central-fd-broker.XXXXXX")"
cleanup() {
  [[ "$tmp" == "${TMPDIR:-/tmp}"/central-fd-broker.* ]] && find "$tmp" -depth -delete
}
trap cleanup EXIT

cc="$(xcrun --find clang)"
cxx="$(xcrun --find clang++)"
sdk="$(xcrun --sdk macosx --show-sdk-path)"
"$cc" -std=c17 -arch arm64 -isysroot "$sdk" -Wall -Wextra -Werror \
  -Wpedantic -I"$dir/include" -fsyntax-only "$dir/probes/abi.c"

common=(-std=c++20 -arch arm64 -isysroot "$sdk" -pthread -Wall -Wextra -Werror
        -Wpedantic -I"$dir/include" "$dir/src/fd_broker.cc"
        "$dir/probes/state_machine_test.cc")
for sanitizer in address undefined thread; do
  binary="$tmp/broker-$sanitizer"
  "$cxx" "${common[@]}" -O1 -g -fno-omit-frame-pointer \
    -fsanitize="$sanitizer" -o "$binary"
  case "$sanitizer" in
    address) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 "$binary" ;;
    undefined) UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$binary" ;;
    thread) TSAN_OPTIONS=halt_on_error=1 "$binary" ;;
  esac
done

if nm -u "$tmp/broker-address" | awk '{print $NF}' |
    grep -E '^_(close|dup|fcntl|ioctl|poll|ppoll|read|recv|send|sendfile|socket|write|dlsym|dlopen)$' \
      >/dev/null; then
  fail 'host descriptor or dynamic-loader dependency escaped broker'
fi
if rg -n '(^|[^.>A-Za-z_])(close|dup|fcntl|ioctl|poll|ppoll|read|recv|send|sendfile|socket|write|dlsym|dlopen)[[:space:]]*\(' \
    "$dir/src" >/dev/null; then
  fail 'host descriptor API entered broker source'
fi
xcrun clang-format --dry-run --Werror "$dir/include/darwin_art_bionic_fd_broker.h" \
  "$dir/src/fd_broker.cc" "$dir/probes/state_machine_test.cc" "$dir/probes/abi.c"
bash -n "$dir/audit.sh"

git -C "$root" diff --check -- tools/bionic-central-fd-broker
git -C "$root" diff --cached --check -- tools/bionic-central-fd-broker
while IFS= read -r -d '' file; do
  set +e
  whitespace="$(git -C "$root" diff --no-index --check /dev/null "$file" 2>&1)"
  status=$?
  set -e
  [[ -z "$whitespace" ]] || fail "untracked whitespace: $file: $whitespace"
  [[ $status -le 1 ]] || fail "could not diff-check untracked file: $file"
done < <(git -C "$root" ls-files --others --exclude-standard -z -- \
         tools/bionic-central-fd-broker)

echo 'bionic-central-fd-broker: PASS C-ABI=v1 token=generation-tagged typed=FS-file+FS-random+stdio+socket dispatch=read+write+poll+sendfile+ioctl close=drained uninstall=quiescent ASan+UBSan+TSan host-fd=0'
