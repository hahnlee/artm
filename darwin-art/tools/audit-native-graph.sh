#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ninja="$root/_aosp/external/skia/third_party/ninja/ninja"
graph_dir="$root/_build/native-graph-audit"
graph="$graph_dir/build.ninja"
mkdir -p "$graph_dir"

if [[ ! -x "$ninja" ]]; then
  echo "native-graph: pinned Ninja is missing: $ninja" >&2
  exit 2
fi

(cd "$root" && cargo run -q -p darwin-art-xtask -- native-graph --out "$graph")

cached_cpp="$(grep -c ': native_cached_cpp ' "$graph" || true)"
cached_archives="$(grep -c ': native_cached_archive ' "$graph" || true)"
icu_cpp="$(grep -c '_build/icu-foundation/objects/.*: native_cached_cpp ' "$graph" || true)"
runtime_cpp="$(grep -c '_build/runtime-bootstrap/objects/.*: native_cached_cpp ' "$graph" || true)"
graphics_cpp="$(grep -c '_build/android-graphics-jni/objects/.*: native_cached_cpp ' "$graph" || true)"

(( icu_cpp >= 458 )) || {
  echo "native-graph: ICU cache incomplete ($icu_cpp/458 TUs)" >&2
  exit 1
}
(( runtime_cpp >= 128 )) || {
  echo "native-graph: ART runtime cache not promotable ($runtime_cpp TUs)" >&2
  exit 1
}
(( graphics_cpp >= 62 )) || {
  echo "native-graph: GraphicsJNI cache incomplete ($graphics_cpp TUs)" >&2
  exit 1
}
grep -q 'depfile = \$out.d' "$graph"
grep -q 'deps = gcc' "$graph"

warm_output="$($ninja -f "$graph" -n icu-foundation 2>&1)"
grep -q 'no work to do' <<<"$warm_output" || {
  echo "native-graph: ICU warm target is not a no-op" >&2
  echo "$warm_output" >&2
  exit 1
}

echo "native-graph: PASS runtime=$runtime_cpp graphics-jni=$graphics_cpp icu=$icu_cpp cached-tu=$cached_cpp archives=$cached_archives warm=no-op depfiles=gcc"
