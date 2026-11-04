#!/bin/bash
# Mutation check of the bench: does run_graph.sh notice when one rule of an optimisation is taken out?
#   mutate.sh <name> <file under core/libs/Audio> <old text> <new text> [base-label] [compile flags]
# Copies core/ util/ synth/ to $OUT/mut_<name>, replaces <old text> by <new text> in the file (CRLF files are handled), builds the bench from
# the copy and compares scenarios $SCS (default "0 1 5") of $NB blocks (default 3000) with the logs of <base-label> (run_graph.sh run <base-label> <sc> <NB> first).
# A mutant that prints BIT IDENTICAL for every scenario is a rule the bench does not exercise.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-/tmp/audio_opt}
NAME=$1; FILE=$2; OLD=$3; NEW=$4; BASE=${5:-base}; FLAGS=${6:--DFW_REVERB_SRAM=1 -DIDLE_NULL}
M=$OUT/mut_$NAME
rm -rf "$M"; mkdir -p "$M"
rsync -a --exclude 'build*' "$ROOT/core" "$ROOT/util" "$ROOT/synth" "$M/"
python3 - "$M/core/libs/Audio/$FILE" "$OLD" "$NEW" <<'PY'
import sys
p, old, new = sys.argv[1:4]
s = open(p, 'rb').read().decode()
if '\r\n' in s:
    old = old.replace('\n', '\r\n'); new = new.replace('\n', '\r\n')
n = s.count(old)
assert n >= 1, f"no match for {old!r}"
open(p, 'wb').write(s.replace(old, new).encode())
print("mutated", n, "place(s)")
PY
"$HERE/run_graph.sh" build "$M" "mut_$NAME" $FLAGS > /dev/null
for sc in ${SCS:-0 1 5}; do
  "$HERE/run_graph.sh" run "mut_$NAME" "$sc" "${NB:-3000}" > /dev/null
  r=$("$HERE/run_graph.sh" cmp "$BASE" "mut_$NAME" "$sc" | grep -E "RESULT|out |queues|meters" | tr '\n' ' ')
  echo "$NAME sc$sc: $r"
done
