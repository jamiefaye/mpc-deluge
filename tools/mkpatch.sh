#!/bin/sh
# Regenerate patches/ from the edits made under third_party/ (run after changing vendored files).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/a"
"$here/tools/pristine.sh" "$tmp/a"
ln -s "$here/third_party" "$tmp/b"
mkdir -p "$here/patches"
cd "$tmp"
for part in deluge argon; do
  out=$here/patches/$part.patch
  # diff exits 1 when there are differences. The sed drops the timestamps diff puts on the
  # ---/+++ lines, so regenerating without changes leaves the patch files byte-identical.
  diff -ruN "a/$part" "b/$part" | sed -E 's/^(---|\+\+\+) ([^	]+)	.*/\1 \2/' > "$out" || true
  if [ -s "$out" ]; then
    echo "patches/$part.patch: $(grep -c '^diff ' "$out") file(s)"
  else
    rm -f "$out"
  fi
done
