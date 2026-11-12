#!/bin/sh
# Write the headers the firmware's CMake build would generate. Usage: gen_headers.sh <dir holding src/>
# Version 1.3.0 is what the firmware's CMakeLists.txt declares at the pinned commit.
set -e
d=$1/src/deluge
subst="-e s/@PROJECT_VERSION_MAJOR@/1/ -e s/@PROJECT_VERSION_MINOR@/3/ -e s/@PROJECT_VERSION_PATCH@/0/"
sed $subst "$d/util/firmware_version.h.in" > "$d/util/firmware_version.h"
sed $subst "$d/version/version.h.in" > "$d/version/version.h"
# The firmware's build writes the version strings from git; here they name the pinned commit.
cat > "$d/version/version.cpp.gen" <<'GEN'
char const* kFirmwareVersionString = "1.3.0-mpc-bef6d9df3";
char const* kFirmwareVersionStringShort = "c1.3.0";

extern "C" {
char const* kCommitShort = "bef6d9df3";
}
GEN
