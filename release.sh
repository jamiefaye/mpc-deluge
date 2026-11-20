#!/bin/sh
# Make the zip that is given to other people: the built plugin with only what is ours to give.
#
#   ./release.sh              build, test, package into dist/
#   ./release.sh --no-build   package what is already built
#
# What is different from what ./deploy.sh installs on Jamie's own device:
# - Content/ holds the repository's own starter presets (plugins/deluge-synth/starter/presets)
#   and nothing else. The project's Content/ and build/library are other people's presets and
#   samples and never go in a package; this script refuses to finish if anything else is found.
# - LICENSE (GPL-3.0) and NOTICE.txt (where the source is, at which commit, and the licences of
#   what is built in) go in the plugin folder, and plugins/deluge-synth/release/README.md beside it.
# Releases are made from a committed tree, so that NOTICE.txt can name the commit.
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=${MPC_SDK:-$here/../mpc-plugin-builder-sdk}
project=plugins/deluge-synth
cd "$here"
. tools/pins.sh

if [ -n "$(git status --porcelain)" ]; then
  echo "The working tree has uncommitted changes: commit first, so the package can name its source." >&2
  exit 1
fi
commit=$(git rev-parse HEAD)
[ "$1" = --no-build ] || ./build.sh "$project"

name=$(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print(j["manufacturer"]+" - VST - "+j["name"])' "$project/plugin.json")
version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$project/plugin.json")
stage=$here/build/release
rm -rf "$stage"
mkdir -p "$stage/build"
cp "$project/plugin.json" "$stage/plugin.json"
cp "$project/release/README.md" "$stage/README.md"
cp -R "$project/build/$name" "$stage/build/$name"
folder=$stage/build/$name

rm -rf "$folder/Content"
mkdir -p "$folder/Content/SYNTHS/Starter" "$folder/Content/SAMPLES"
cp "$project"/starter/presets/*.XML "$folder/Content/SYNTHS/Starter/"
cat > "$folder/Content/SAMPLES/README.txt" <<TXT
Samples for your own Deluge presets go here, at the paths they have under SAMPLES on a Deluge
SD card. See README.md in the package.
TXT
rm -f "$folder"/diagnostics*.txt "$folder"/diagnostics-on "$folder"/missing-samples.txt
cp LICENSE "$folder/LICENSE"
# The user's README inside the plugin folder as well as beside it: the Open MPC catalog's zips
# carry the folder and nothing beside it.
cp "$project/release/README.md" "$folder/README.md"
cat > "$folder/NOTICE.txt" <<TXT
Downrush $version for Akai MPC and Force
Copyright (C) the Downrush contributors and the authors of the code it is built from.

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. It is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See LICENSE in this folder.

Source code for exactly this build:
    https://github.com/jamiefaye/mpc-deluge   commit $commit
The repository holds this project's own code, the scripts that fetch the code below at the
versions named, and the changes made to it (patches/).

Built from:
  Deluge community firmware $fw_version (GPL-3.0-or-later)
      https://github.com/SynthstromAudible/DelugeFirmware   commit $fw_commit
      Copyright (C) Synthstrom Audible Limited and the firmware's contributors.
  argon (MIT licence), https://github.com/stellar-aria/argon   commit $argon_commit
  Embedded Template Library $etl_tag (MIT licence), https://github.com/ETLCPP/etl
  MPC Plugin Builder SDK framework (MIT licence), https://github.com/inmusic-software/mpc-plugin-builder-sdk

The presets in Content/SYNTHS/Starter are part of this project and under the same licence.

Not made, endorsed or supported by Synthstrom Audible or by inMusic / Akai.
TXT

# Nothing but ours: the starter presets, the note in SAMPLES, and no audio anywhere.
extra=$(cd "$folder/Content" && find . -type f ! -path './SYNTHS/Starter/*.XML' ! -path './SAMPLES/README.txt')
count=$(ls "$folder/Content/SYNTHS/Starter" | wc -l | tr -d ' ')
ours=$(ls "$project/starter/presets" | wc -l | tr -d ' ')
if [ -n "$extra" ] || [ "$count" != "$ours" ]; then
  echo "Content/ holds something that is not a starter preset; not packaging:" >&2
  echo "$extra" >&2
  exit 1
fi

mkdir -p dist
python3 "$sdk/tools/package.py" "$stage" --out "$here/dist"
# Named as the Open MPC catalog wants a zip in Akai's one-folder format: <Name>-<version>-mpc-sdk.zip
# (catalog/README.md in open-mpc/mpc-vst-plugins). It is the same zip whoever it is for.
made=$(ls -t dist/*.zip | head -1)
zip=dist/$(python3 -c 'import json,re,sys; j=json.load(open(sys.argv[1])); print(re.sub(r"[^A-Za-z0-9]+", "-", j["name"]).strip("-") + "-" + j["version"] + "-mpc-sdk.zip")' "$project/plugin.json")
[ "$made" = "$zip" ] || mv "$made" "$zip"
shasum -a 256 "$zip"
echo "Contents other than the skin:"
unzip -l "$zip" | grep -v "Plugin Skins/" | sed -n '4,$p'
