#!/bin/sh
# Regenerate patches/ from the edits made under third_party/ (run after changing vendored files).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
fw=${DELUGE_FW:-$HOME/deluge/DelugeFirmware}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/a/deluge" "$tmp/a/argon"
git -C "$fw" archive HEAD src LICENSE | tar -x -C "$tmp/a/deluge"
cp -R "$fw/build/_deps/argon-src/include" "$tmp/a/argon/include"
cp "$fw/build/_deps/argon-src/README.md" "$tmp/a/argon/README.md"
"$here/tools/gen_headers.sh" "$tmp/a/deluge"
ln -s "$here/third_party" "$tmp/b"
mkdir -p "$here/patches"
cd "$tmp"
for part in deluge argon; do
  out=$here/patches/$part.patch
  diff -ruN "a/$part" "b/$part" > "$out" || true   # diff exits 1 when there are differences
  if [ -s "$out" ]; then
    echo "patches/$part.patch: $(grep -c '^diff ' "$out") file(s)"
  else
    rm -f "$out"
  fi
done
