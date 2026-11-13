#!/bin/sh
# Fill third_party/ with the upstream sources at the pinned versions (tools/pins.sh) and apply
# patches/*.patch. third_party/ is generated; the patches are the source.
#   tools/vendor.sh            refresh third_party/ from scratch
#   DELUGE_FW=/path tools/vendor.sh     the local DelugeFirmware clone, if not ~/deluge/DelugeFirmware
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tp=$here/third_party
"$here/tools/pristine.sh" "$tp"
for p in "$here"/patches/*.patch; do
  [ -f "$p" ] || continue
  echo "applying $(basename "$p")"
  patch -s -p1 -d "$tp" < "$p"
done
. "$here/tools/pins.sh"
echo "third_party/ ready (firmware $fw_commit)"
