#!/bin/sh
# Copy the Deluge firmware source and the Argon NEON library into third_party/ at the pinned
# commits, then apply patches/*.patch. third_party/ is generated; the patches are the source.
#   tools/vendor.sh            refresh third_party/ from scratch
#   DELUGE_FW=/path tools/vendor.sh
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
fw=${DELUGE_FW:-$HOME/deluge/DelugeFirmware}
fw_commit=bef6d9df3f627f24fc3057a9efe15b567e234d83      # SynthstromAudible/DelugeFirmware, community
argon_commit=3779c5315a978a2f8c892e320ac12ecbb77c3759   # stellar-aria/argon, as pinned by the firmware

have=$(git -C "$fw" rev-parse HEAD)
if [ "$have" != "$fw_commit" ]; then
  echo "warning: $fw is at $have, patches were made against $fw_commit" >&2
fi
argon=$fw/build/_deps/argon-src
if [ ! -d "$argon/include" ]; then
  echo "Argon not found at $argon (configure the firmware build once, or clone stellar-aria/argon there)" >&2
  exit 1
fi
if [ "$(git -C "$argon" rev-parse HEAD)" != "$argon_commit" ]; then
  echo "warning: Argon is not at $argon_commit" >&2
fi

tp=$here/third_party
etl=$fw/build/_deps/etl-src                             # ETLCPP/etl 20.40.0, as pinned by the firmware
rm -rf "$tp/deluge" "$tp/argon" "$tp/etl"
mkdir -p "$tp/deluge" "$tp/argon" "$tp/etl"
# Tracked firmware sources only (no build output, no local files).
git -C "$fw" archive "$have" src LICENSE | tar -x -C "$tp/deluge"
cp -R "$argon/include" "$tp/argon/include"
cp "$argon/README.md" "$tp/argon/README.md"
cp -R "$etl/include" "$tp/etl/include"
cp "$etl/LICENSE" "$tp/etl/LICENSE"

"$here/tools/gen_headers.sh" "$tp/deluge"

for p in "$here"/patches/*.patch; do
  [ -f "$p" ] || continue
  echo "applying $(basename "$p")"
  patch -s -p1 -d "$tp" < "$p"
done
echo "third_party/ ready (firmware $have)"
