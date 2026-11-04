#!/bin/sh
# Flash budget of the SYNTH_OPEN engines against FLASH_IMG_MAX (1,044,480 B, text+data of the image).
# Normally run through make:
#   make synth-engines [SYNTH_ENGINES="ACD VAP WTFM CHIP"]   cost of every engine and what fits on top of the selection
#   make original SYNTH_OPEN=1 ...                            prints the report and writes build/BUILD.txt after the link
# Direct use: engine_budget.sh report|table <options>, see engine_budget.py (TOOLCHAIN = arm-none-eabi- prefix).
exec python3 "$(dirname "$0")/engine_budget.py" "$@"
