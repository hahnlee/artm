#!/bin/bash
set -euo pipefail
export LC_ALL=C

project_root="$(cd "$(dirname "$0")/.." && pwd)"
stage="$(mktemp -d "${TMPDIR:-/tmp}/android-jni-trampoline.XXXXXX")"
trap 'rm -rf "$stage"' EXIT

xcrun clang++ -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$project_root/compat" \
  "$project_root/compat/darwin_android_jni_trampoline.cc" \
  "$project_root/tools/android-jni-trampoline-smoke.cc" \
  -o "$stage/android-jni-trampoline-smoke"

output="$("$stage/android-jni-trampoline-smoke")"
grep -F 'android-jni-trampoline: PASS' <<< "$output" >/dev/null
printf '%s\n' "$output"
