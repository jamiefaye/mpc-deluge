#!/bin/sh
# Build a plugin with the MPC Plugin SDK, then run it through the SDK's host simulator.
#   ./build.sh                 build and test plugins/hello
#   ./build.sh plugins/other   another project
#   MPC_SDK=/path/to/sdk ./build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=${MPC_SDK:-$here/../mpc-plugin-builder-sdk}
project=${1:-plugins/hello}

if [ ! -f "$sdk/tools/build.py" ]; then
  echo "MPC Plugin SDK not found at $sdk (set MPC_SDK, or clone it beside this folder)" >&2
  exit 1
fi

cd "$here"
MPC_SDK="$sdk" python3 "$here/tools/build.py" "$project"

# Things the SDK's validator and the simulator cannot see, both learned on a Force:
# a non-glibc import stops the plugin loading; an alignment-demanding NEON access can crash it.
if command -v arm-none-eabi-readelf >/dev/null 2>&1; then
  "$here/tools/check_imports.sh" "$project"/build/*/VST/*.so
  "$here/tools/check_alignment.py" "$project"/build/*/VST/*-arm32.so || echo "      (review the accesses listed above; see CLAUDE.md, \"Alignment on arm32\")"
else
  echo "warning: arm-none-eabi binutils not found; skipping the import and alignment checks" >&2
fi

case $(uname) in Darwin) ext=dylib ;; *) ext=so ;; esac
for lib in "$project"/build/native/*."$ext"; do
  "$sdk/.sdk-tools/host_sim" "$lib" test --wav "$project/build/test.wav"
done

# The same simulator, built for 32-bit ARM, against the arm32 .so in an emulated machine: the
# closest thing to a Force or MPC Live that runs here. Skipped if qemu is not installed.
if command -v qemu-system-arm >/dev/null 2>&1; then
  echo
  echo "== arm32 in the emulator (tools/armtest) =="
  "$here/tools/armtest/run.py" "$project" | grep -E '^(PASS|FAIL|WARN|CRASH|Result|INFO  (main pass|block of)|       )' || true
  # run.py's own exit status is lost in the pipe; read the verdict from its log instead
  if ! grep -q '^Result: PASS' "$here/build/armtest/console.log" || grep -q '^CRASH' "$here/build/armtest/console.log"; then
    echo "arm32 emulator run failed (see build/armtest/console.log)" >&2
    exit 1
  fi
else
  echo "warning: qemu-system-arm not found; skipping the arm32 emulator run" >&2
fi
