#!/bin/bash
# Flash fit table for `make original SYNTH_OPEN=1` (host toolchain ./agt/10.3).
# Usage: synth-open/tools/fit_variants.sh [variant...]    variants: a b c d (default: all)
#   a  all engines, current flags (-O2)
#   b  engine files -Os
#   c  b + cold synth GUI files -Os (the default of `make original SYNTH_OPEN=1`)
#   d  c + the shim (open_synth.cpp) -Os, informational: the shim holds the render path
# Each variant has its own core build dir (SYNTH_TAG), logs go to /tmp/fit_<variant>.log.
cd "$(dirname "$0")/../.." || exit 1
variants=("$@"); [ ${#variants[@]} -eq 0 ] && variants=(a b c d)
for v in "${variants[@]}"; do
  case $v in
    a) args=(SYNTH_OPT_ENGINES= SYNTH_OPT_GUI=) ;;
    b) args=(SYNTH_OPT_ENGINES=-Os SYNTH_OPT_GUI=) ;;
    c) args=(SYNTH_OPT_ENGINES=-Os SYNTH_OPT_GUI=-Os) ;;
    d) args=(SYNTH_OPT_ENGINES=-Os SYNTH_OPT_GUI=-Os SYNTH_OPT_SHIM=-Os) ;;
    *) args=($v) ;;   # anything else: raw make args, e.g. "SYNTH_OPT_SHIM=-Os"
  esac
  rm -f platforms/mk66/build/PolyendTracker.axf
  make original SYNTH_OPEN=1 SYNTH_TAG=-$v "${args[@]}" > /tmp/fit_$v.log 2>&1
  rc=$?
  flash=$(grep -E '^ +FLASH:' /tmp/fit_$v.log | tail -1)
  over=$(grep -o "overflowed by [0-9]* bytes" /tmp/fit_$v.log | tail -1)
  echo "variant $v rc=$rc | $flash | $over"
done
