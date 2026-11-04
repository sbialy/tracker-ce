#!/bin/sh
# Renders PHZ demo WAV files on the host (see phz_render.cpp for the list), so the engine can be listened to without
# the device. Needs clang++ (macOS: x86_64 build via Rosetta, like run_smoke.sh).
#   synth-open/tools/phz_render.sh [outdir]      default build/phz-wav
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/phz-wav}
mkdir -p "$OUT"
BIN=${TMPDIR:-/tmp}/phz_render_$$
clang++ -arch x86_64 -std=c++20 -O2 -ffp-contract=off "-D_GLIBCXX_THROW_OR_ABORT(x)=__builtin_abort()" \
  -I "$ROOT/synth/include" -I "$ROOT/util/include" -I "$ROOT/util/3rdparty" -I "$ROOT/synth-open" -I "$ROOT/synth-open/ot" \
  -DFEATURE_SYNTH -DFEATURE_SYNTH_OPEN -DSYNTH_ENGINE_PHZ \
  "$ROOT/synth-open/tools/phz_render.cpp" "$ROOT/synth-open/phz/phz.cpp" "$ROOT/synth-open/engine_phz.cpp" -o "$BIN"
"$BIN" "$OUT"
rm -f "$BIN"
echo "WAV files in $OUT"
