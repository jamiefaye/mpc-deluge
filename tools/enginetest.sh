#!/bin/sh
# Build the firmware library and tests/engine_smoke.cpp, run it in the emulated arm32 machine on
# one SYNTH preset, and if it crashed say where.
#   tools/enginetest.sh [preset file]        default: ~/deluge/SDCard/SYNTHS/SYNT100.XML
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
preset=${1:-$HOME/deluge/SDCard/SYNTHS/SYNT100.XML}
cd "$here"
tools/build_firmware.py arm32 --test tests/engine_smoke.cpp | tail -2
tools/armtest/run.py --exe build/firmware/arm32/engine_smoke /sd "SYNTHS/$(basename "$preset")" \
    --file "$preset=/sd/SYNTHS/$(basename "$preset")" 2>&1 | grep -v -E '^(\[ *[0-9.]+\]|CRASH-(PC|LR|STACK))' | tail -${LINES_SHOWN:-14} || true
if grep -q '^CRASH-PC' build/armtest/console.log; then
  tools/armtest/symbolize.py build/firmware/arm32/engine_smoke | head -14
fi
