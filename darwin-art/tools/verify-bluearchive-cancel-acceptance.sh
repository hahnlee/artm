#!/usr/bin/env bash
set -euo pipefail

# Read-only verifier for a completed, unchanged-APK run. It sends no input and
# never presses Confirm or starts a download. Human review of the linked PNGs
# remains required; OCR proves panel text, not arbitrary game semantics.
log="${1:?usage: $0 RUN_LOG ABSOLUTE_SCANOUT_PREFIX}"
prefix="${2:?missing absolute scanout prefix}"
[[ -f "$log" && "$prefix" == /* ]] || exit 66
for tool in tesseract magick; do command -v "$tool" >/dev/null; done
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/_build/bluearchive-cancel-verification"
output="$(mktemp -d "$root/_build/bluearchive-cancel-verification/run.XXXXXX")"

# Require the targeted Cancel DOWN, the explicitly labeled synthetic hold,
# and its matching UP, in that order. consumed alone is not the UI gate.
read -r down_line up_line < <(awk '
  /MotionEvent target x=514 y=504 / { target=NR }
  target && /MotionEvent ABI2 action=0 consumed=1 / { down=NR }
  down && /synthetic tap x=257 y=252 hold_requested_ms=80 / { tap=1 }
  tap && /MotionEvent ABI2 action=1 consumed=1 / { print target, NR; exit }
' "$log")
[[ "$down_line" =~ ^[0-9]+$ && "$up_line" =~ ^[0-9]+$ ]]
before="$(awk -v stop="$down_line" -v prefix="$prefix" '
  NR < stop && /diagnostic scanout .*written=1 path=/ {
    sub(/^.*path=/, ""); if (index($0,prefix)==1) last=$0
  } END { print last }
' "$log")"
[[ -f "$before" ]]
tesseract "$before" "$output/before" 2>"$output/ocr.stderr"
grep -F 'Notice' "$output/before.txt" >/dev/null
grep -F 'Ready to download' "$output/before.txt" >/dev/null

after=""
while IFS= read -r candidate; do
  [[ -f "$candidate" ]] || continue
  tesseract "$candidate" "$output/after" 2>>"$output/ocr.stderr"
  if grep -F 'Notice' "$output/after.txt" >/dev/null; then continue; fi
  metrics="$(magick identify -format '%[fx:mean] %[fx:standard_deviation]' "$candidate")"
  if awk '{ exit !($1 > 0.05 && $2 > 0.02) }' <<<"$metrics"; then
    after="$candidate"
    break
  fi
done < <(awk -v start="$up_line" -v prefix="$prefix" '
  NR > start && /diagnostic scanout .*written=1 path=/ {
    sub(/^.*path=/, ""); if (index($0,prefix)==1) print
  }
' "$log")
[[ -n "$after" ]] || { echo 'FAIL: no nonblank post-UP frame without Notice'; exit 1; }
grep -a -F 'nativeRender exit result=1 exception=0' "$log" >/dev/null
{
  printf 'PASS synthetic-Cancel DOWN/UP consumed=1 Notice-before=present Notice-after=absent nonblank-after=yes\n'
  printf 'before=%s\nafter=%s\nlog=%s\n' "$before" "$after" "$log"
  printf 'Manual visual review required; not a physical-click or gameplay claim.\n'
  magick identify -format '%f mean=%[fx:mean] std=%[fx:standard_deviation]\n' "$before" "$after"
  shasum -a 256 "$log" "$before" "$after"
} | tee "$output/result.txt"
printf 'Verification artifacts: %s\n' "$output"
