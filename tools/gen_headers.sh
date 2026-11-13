#!/bin/sh
# Write the headers the firmware's CMake build would generate. Usage: gen_headers.sh <dir holding src/>
# The version numbers and commit come from tools/pins.sh.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
. "${PINS:-$here/tools/pins.sh}"
d=$1/src/deluge
major=${fw_version%%.*}; rest=${fw_version#*.}; minor=${rest%%.*}; patch=${rest#*.}
short=$(printf %.9s "$fw_commit")
subst="-e s/@PROJECT_VERSION_MAJOR@/$major/ -e s/@PROJECT_VERSION_MINOR@/$minor/ -e s/@PROJECT_VERSION_PATCH@/$patch/"
sed $subst "$d/util/firmware_version.h.in" > "$d/util/firmware_version.h"
sed $subst "$d/version/version.h.in" > "$d/version/version.h"
# The firmware's build writes the version strings from git; here they name the pinned commit.
cat > "$d/version/version.cpp.gen" <<GEN
char const* kFirmwareVersionString = "$fw_version-mpc-$short";
char const* kFirmwareVersionStringShort = "c$fw_version";

extern "C" {
char const* kCommitShort = "$short";
}
GEN
