#!/bin/sh
# Fail if an ARM plugin binary needs a function that glibc does not provide. The MPC resolves
# every symbol when it loads a plugin, so one stray import (for example __atomic_load from
# libatomic) means the plugin silently fails to load: no crash, no sound.
#   tools/check_imports.sh <plugin>.so ...        needs arm-none-eabi-readelf (it reads any ARM ELF)
status=0
for so in "$@"; do
  missing=$(arm-none-eabi-readelf --dyn-syms -W "$so" 2>/dev/null \
      | awk '$7=="UND" && $8!="" && $8 !~ /@GLIBC_/ {print $8}' | sort -u | tr '\n' ' ')
  if [ -n "$missing" ]; then
    echo "FAIL  $(basename "$so") imports symbols the MPC cannot supply: $missing" >&2
    status=1
  else
    echo "OK    $(basename "$so") imports only glibc symbols"
  fi
done
exit $status
