#!/bin/bash
# Sizing probe for the full-engine port: compile every firmware .cpp for one target, then link
# root.cpp (load a patch, play a note, render) against all of it with unused code dropped, and
# list the symbols still missing. Output goes to build/sizing/<target>/.
#   tools/sizing/run.sh arm32     (the MPC/Force target; default)
#   tools/sizing/run.sh native    (arm64 macOS)
# The first measuring tool (2026-11-12), from before hosted/ existed. tools/build_firmware.py
# builds the real library and its --probe does the same link; use that.
set -u
here=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${MPC_SDK:-$here/../mpc-plugin-builder-sdk}
zig="$sdk/.sdk-tools/zigenv/bin/python -m ziglang"
tp=$here/third_party
which=${1:-arm32}
case $which in
  arm32)  target="-target arm-linux-gnueabihf.2.31 -mcpu=cortex_a17" ;;
  arm64)  target="-target aarch64-linux-gnu.2.31 -mcpu=cortex_a55" ;;
  native) target="-target aarch64-macos" ;;
  *) echo "unknown target $which" >&2; exit 1 ;;
esac
out=$here/build/sizing/$which
rm -rf "$out"; mkdir -p "$out/obj"
flags="-std=c++23 -O1 -fPIC -c -w -ffunction-sections -fdata-sections -fsigned-char -D_GNU_SOURCE -DUSE_TASK_MANAGER -DDELUGE_HOSTED=1 -DPRINTF_ALIAS_STANDARD_FUNCTION_NAMES=0 $target"
inc="-I$here/hosted -Isrc -Isrc/deluge -Isrc/OSLikeStuff -Isrc/lib -Isrc/NE10/inc -Isrc/fatfs -Isrc/deluge/version -I$tp/argon/include -I$tp/etl/include"
cd "$tp/deluge"
export zig flags inc out
compile_one() {
  f=$1; o=$out/obj/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$//')
  if $zig c++ $flags $inc "src/deluge/$f" -o "$o.o" > "$o.err" 2>&1; then echo "ok $f"; else echo "FAIL $f"; fi
}
export -f compile_one
(cd src/deluge && find . -name '*.cpp' | sed 's#^\./##' | sort) | xargs -P "$(getconf _NPROCESSORS_ONLN)" -I{} bash -c 'compile_one {}' > "$out/compile.txt"
echo "compiled: $(grep -c '^ok' "$out/compile.txt") of $(wc -l < "$out/compile.txt" | tr -d ' ')  (failures: grep FAIL $out/compile.txt; messages in obj/*.err)"
[ "$which" = native ] && exit 0
$zig c++ $flags $inc "$here/tools/sizing/root.cpp" -o "$out/root.o" || exit 1
$zig c++ $target -nostartfiles -Wl,-e,sizing_root -Wl,--gc-sections -o "$out/probe" "$out/root.o" "$out"/obj/*.o > "$out/link.log" 2>&1
grep 'undefined symbol' "$out/link.log" | sed 's/.*undefined symbol: //' | sort -u > "$out/undefined.txt"
echo "undefined symbols after dropping unused code: $(wc -l < "$out/undefined.txt" | tr -d ' ')  ($out/undefined.txt)"
