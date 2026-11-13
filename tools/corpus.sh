#!/bin/sh
# Run every SYNTH preset on a Deluge card through the engine on all three targets and check that
# they produce the same audio, sample for sample.
#
#   tools/corpus.sh [card folder]         default: ~/deluge/SDCard
#
# Targets: 32-bit ARM Linux in the emulator (the Force and MPC Live family), 64-bit ARM Linux in a
# virtual machine (the Live III family), and this computer. Two passes on each
# (tests/engine_corpus.cpp): every preset in a process of its own, and every preset one after
# another in one process, which is how a plugin is used and what exercises memory management.
# 32-bit ARM is the reference; any preset whose checksum differs on another target is listed.
#
# The second pass runs with DELUGE_SCRUB set (hosted.h): newly allocated memory is filled with a
# fixed byte. The firmware reads a little memory before writing it (at note-on a voice patches
# from envelope levels it has not set yet, so it uses what the last occupant of that memory left
# there), and what is left there differs between 32-bit and 64-bit builds because their objects
# differ in size. Without the fill a handful of presets differ for that reason alone.
# Results are left in build/corpus/. Takes about ten minutes, nearly all of it the 32-bit emulator.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
card=${1:-$HOME/deluge/SDCard}
out=$here/build/corpus
cd "$here"
[ -d "$card/SYNTHS" ] || { echo "no SYNTHS folder in $card" >&2; exit 1; }

# Only the samples the presets use: the emulated machines hold their files in memory.
samples=$out/sd/SAMPLES
if [ ! -d "$samples" ]; then
  echo "gathering the samples the presets refer to into build/corpus/sd"
  for preset in "$card"/SYNTHS/*.XML "$card"/SYNTHS/*.xml; do
    [ -f "$preset" ] || continue
    tools/preset_files.py "$preset" | while IFS= read -r name; do
      case $name in SAMPLES/*) ;; *) continue ;; esac
      if [ -f "$card/$name" ] && [ ! -f "$out/sd/$name" ]; then
        mkdir -p "$(dirname "$out/sd/$name")"
        cp "$card/$name" "$out/sd/$name"
      fi
    done
  done
fi
mkdir -p "$samples" "$out/root"
ln -sfn "$card/SYNTHS" "$out/root/SYNTHS"
ln -sfn "$samples" "$out/root/SAMPLES"

for target in arm32 arm64 native; do
  tools/build_firmware.py $target --test tests/engine_corpus.cpp | tail -1
done

files="--file $card/SYNTHS=/sd/SYNTHS --file $samples=/sd/SAMPLES"
status=0
for mode in each switch; do
  arg=; env=; scrub=
  if [ $mode = switch ]; then
    arg=switch; scrub=DELUGE_SCRUB=165; env="--env $scrub"
  fi
  echo "== pass: $([ $mode = each ] && echo 'each preset in its own process' || echo 'all presets in one process, new memory pre-filled')"
  # shellcheck disable=SC2086
  ARMTEST_TIMEOUT=1800 tools/armtest/run.py $env --exe build/firmware/arm32/engine_corpus /sd SYNTHS $arg $files > "$out/$mode-arm32.txt" 2>&1 || true
  # shellcheck disable=SC2086
  tools/armtest/run.py --arm64 $env --exe build/firmware/arm64/engine_corpus /sd SYNTHS $arg $files > "$out/$mode-arm64.txt" 2>&1 || true
  # shellcheck disable=SC2086
  env $scrub build/firmware/native/engine_corpus "$out/root" SYNTHS $arg > "$out/$mode-native.txt" 2>&1 || true
  for target in arm32 arm64 native; do
    printf '%-7s %s\n' "$target" "$(grep '^CORPUS' "$out/$mode-$target.txt" || echo 'did not finish (see the file)')"
    grep -q '^CORPUS' "$out/$mode-$target.txt" || status=1
  done
  for target in arm64 native; do
    printf '%-7s against arm32: ' "$target"
    tools/corpus_compare.py "$out/$mode-arm32.txt" "$out/$mode-$target.txt" || status=1
  done
done
exit $status
