#!/bin/sh
# Fill a folder with the unmodified upstream sources this project builds on, at the pinned
# versions: <dest>/deluge (the firmware's src/ and LICENSE), <dest>/argon, <dest>/etl.
# tools/vendor.sh applies patches/ on top of this; tools/mkpatch.sh diffs against it.
#
#   tools/pristine.sh <dest>
#
# The firmware comes from a local clone of SynthstromAudible/DelugeFirmware (DELUGE_FW, default
# ~/deluge/DelugeFirmware): the pinned tag is read with `git archive`, so that clone's own
# checkout and branch are left alone. It only needs to have the tag (`git fetch --tags`).
# Argon and ETL are cloned from GitHub at the versions the firmware's lib/CMakeLists.txt pins and
# kept in build/upstream/, so nothing depends on the firmware's own build having been run.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
dest=$1
[ -n "$dest" ] || { echo "usage: tools/pristine.sh <dest>" >&2; exit 1; }

. "${PINS:-$here/tools/pins.sh}"   # PINS: another pins file, for comparing versions

fw=${DELUGE_FW:-$HOME/deluge/DelugeFirmware}
if ! git -C "$fw" rev-parse -q --verify "$fw_commit^{commit}" >/dev/null 2>&1; then
  echo "$fw does not have firmware commit $fw_commit ($fw_version)." >&2
  echo "Clone SynthstromAudible/DelugeFirmware there (or set DELUGE_FW) and run: git fetch --tags" >&2
  exit 1
fi

# clone_at <name> <url> <commit or tag>: a clone in build/upstream/<name> with that version checked out
clone_at() {
  dir=$here/build/upstream/$1
  if [ ! -d "$dir/.git" ]; then
    mkdir -p "$here/build/upstream"
    git clone -q "$2" "$dir"
  fi
  if ! git -C "$dir" rev-parse -q --verify "$3^{commit}" >/dev/null 2>&1; then
    git -C "$dir" fetch -q --tags origin
  fi
  git -C "$dir" -c advice.detachedHead=false checkout -q "$3"
}
clone_at argon https://github.com/stellar-aria/argon "$argon_commit"
clone_at etl https://github.com/ETLCPP/etl "$etl_tag"

rm -rf "$dest/deluge" "$dest/argon" "$dest/etl"
mkdir -p "$dest/deluge" "$dest/argon" "$dest/etl"
git -C "$fw" archive "$fw_commit" src LICENSE | tar -x -C "$dest/deluge"
cp -R "$here/build/upstream/argon/include" "$dest/argon/include"
cp "$here/build/upstream/argon/README.md" "$dest/argon/README.md"
cp -R "$here/build/upstream/etl/include" "$dest/etl/include"
cp "$here/build/upstream/etl/LICENSE" "$dest/etl/LICENSE"
"$here/tools/gen_headers.sh" "$dest/deluge"
