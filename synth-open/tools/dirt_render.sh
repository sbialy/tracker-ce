#!/bin/sh
# Renders the live Dirt insert on the host: a synthetic loop dry and through each Saturation list entry (Dirt Soft / Hard / Fold /
# Asym / Crush) as 16 bit WAV files, plus self checks (wet 0 is bit exact, full scale input and silence stay finite). Same per block
# code as the firmware (synth-open/ff_dirt.cpp, ff_dirt_ctl.cpp), plain float instead of the ARM asm. Needs clang++.
#   synth-open/tools/dirt_render.sh [outdir]      default build/dirt-wav
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/dirt-wav}
mkdir -p "$OUT"
BIN=${TMPDIR:-/tmp}/dirt_render_$$
clang++ -std=c++20 -O2 -ffp-contract=off -I "$ROOT/synth-open" \
  "$ROOT/synth-open/tools/dirt_render.cpp" "$ROOT/synth-open/ff_dirt.cpp" "$ROOT/synth-open/ff_dirt_ctl.cpp" -o "$BIN"
"$BIN" "$OUT"
rm -f "$BIN"
echo "WAV files in $OUT"
