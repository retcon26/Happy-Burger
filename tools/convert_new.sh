#!/bin/bash
# Converts maps from tools/maps.txt that haven't been converted yet, several at once, then refreshes
# the UI pack (for their thumbnails). One failing map doesn't stop the rest.
# Usage: tools/convert_new.sh [parallel jobs, default: half the CPU cores] [all]
#   "all" reconverts every map, e.g. after the map format changes.
cd "$(dirname "$0")"
JOBS="${1:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}"
FORCE="$2"

convert_one() {
  line="$1"
  name="$(basename "$line" .bsp)"
  case "$line" in */Half-Life/*) name="hl1_$name" ;; esac
  if python3 -m bspconv convert "$line" > "/tmp/conv_$name.log" 2>&1; then
    echo "OK   $name  $(grep 'wrote' "/tmp/conv_$name.log" | head -1 | sed 's/.*: //')"
  else
    echo "FAIL $name  $(tail -n1 "/tmp/conv_$name.log")"
  fi
}
export -f convert_one

grep -v '^\s*#' maps.txt | sed 's/[[:space:]]*$//' | grep -v '^$' | while IFS= read -r line; do
  name="$(basename "$line" .bsp)"
  case "$line" in */Half-Life/*) name="hl1_$name" ;; esac
  [ "$FORCE" != "all" ] && [ -f "../psp/data/maps/$name.pmap" ] && [ -f "../psp/data/maps/$name.low.pmap" ] && continue
  printf '%s\0' "$line"
done | xargs -0 -P "$JOBS" -I{} bash -c 'convert_one "$1"' _ {}

python3 -m bspconv ui | grep 'ui:'
echo "All done."
