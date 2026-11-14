#!/bin/sh
# Build tests/plugin_grid.cpp (a small VST2 host) and run it against the native build of Deluge
# Synth: how the gold knobs and grid pads follow each other, and that a saved state comes back.
# Build the plugin first (./build.sh plugins/deluge-synth). The real engine is only in the native
# build on a 64-bit ARM computer; on Intel this exercises the placeholder.
#   tools/plugintest.sh [preset name]
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
sdk=${MPC_SDK:-$here/../mpc-plugin-builder-sdk}
case $(uname) in Darwin) ext=dylib ;; *) ext=so ;; esac
lib=$here/plugins/deluge-synth/build/native/deluge-synth.$ext
[ -f "$lib" ] || { echo "no $lib: run ./build.sh plugins/deluge-synth" >&2; exit 1; }
mkdir -p "$here/build/plugintest"
c++ -std=c++17 -O1 -I"$sdk/framework" -I"$here/plugins/deluge-synth/src" \
  -o "$here/build/plugintest/plugin_grid" "$here/tests/plugin_grid.cpp" -ldl -lpthread
"$here/build/plugintest/plugin_grid" "$lib" "$@"
