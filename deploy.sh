#!/bin/sh
# Build a plugin, test it in the host simulator, and install it on an MPC over the network with
# the SDK's mpc_remote.py (the MPC's developer mode must be on; see CLAUDE.md, "Deploying").
#
#   ./deploy.sh plugins/deluge-proto              build, test, install
#   ./deploy.sh plugins/deluge-proto --restart    ... then restart the MPC app so it rescans
#   ./deploy.sh --info | --doctor | --list        talk to the MPC without building
#   ./deploy.sh plugins/deluge-proto --no-build   install what is already in build/
#   ./deploy.sh plugins/deluge-synth --content build/library
#                                                 install with <folder>/Content in place of the
#                                                 project's Content/ (a full preset library; see
#                                                 tools/organize_library.py). Presets saved on the
#                                                 device (Content/SYNTHS/User) are kept.
#
# The MPC only loads a new binary when its app starts, so a changed plugin needs --restart (or a
# power cycle). RESTARTING DISCARDS ANY UNSAVED PROJECT ON THE MPC.
#
# Settings come from the environment or from .mpc.env beside this script (gitignored):
#   MPC_ADDRESS=192.168.1.50     IP address or host name of the MPC or Force
#   MPC_DRIVE=1                  drive number, name or mount point (see ./deploy.sh --info)
#   MPC_SSH_PASSWORD=...         only if you have set one on the device
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=${MPC_SDK:-$here/../mpc-plugin-builder-sdk}
# What is set in the environment wins over .mpc.env, so one deploy can go to another device or
# drive: MPC_DRIVE="MPCstuff HD" ./deploy.sh plugins/deluge-synth
given_address=$MPC_ADDRESS
given_drive=$MPC_DRIVE
[ -f "$here/.mpc.env" ] && . "$here/.mpc.env"
[ -n "$given_address" ] && MPC_ADDRESS=$given_address
[ -n "$given_drive" ] && MPC_DRIVE=$given_drive
[ -n "$MPC_SSH_PASSWORD" ] && export MPC_SSH_PASSWORD

if [ -z "$MPC_ADDRESS" ]; then
  echo "Set MPC_ADDRESS (in the environment or in $here/.mpc.env)." >&2
  echo "The address is in the MPC's Preferences, on the Wi-Fi tab (hold Shift) or the Ethernet tab." >&2
  exit 1
fi
remote() { python3 "$sdk/tools/mpc_remote.py" "$MPC_ADDRESS" --no-color "$@"; }

project=
restart=no
build=yes
pull=no
content=
want_content=no
for arg in "$@"; do
  if [ "$want_content" = yes ]; then content=$arg; want_content=no; continue; fi
  case $arg in
    --content) want_content=yes ;;
    --info)    remote info; exit ;;
    --doctor)  remote doctor; exit ;;
    --list)    remote list ${MPC_DRIVE:+--drive "$MPC_DRIVE"}; exit ;;
    --restart) restart=yes ;;
    --no-build) build=no ;;
    -*) echo "unknown option $arg" >&2; exit 1 ;;
    *) project=$arg ;;
  esac
done
if [ -z "$project" ]; then
  echo "usage: ./deploy.sh plugins/<name> [--restart] [--no-build]   (or --info, --doctor, --list)" >&2
  exit 1
fi
if [ -z "$MPC_DRIVE" ]; then
  echo "Set MPC_DRIVE; the drives on the device are:" >&2
  remote drives >&2
  exit 1
fi

cd "$here"

# Akai's layout keeps a plugin's files, including ones the user added on the device, inside the
# plugin folder (Content/), and installing replaces that folder. So first bring back whatever is
# in the installed copy's Content/ and merge it into the project's Content/ (never overwriting a
# local file); the build then carries it into the new copy. Skipped when nothing is installed.
if [ -z "$content" ] && { [ -d "$project/Content" ] || [ "$pull" = yes ]; }; then
  name=$(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print(j["manufacturer"]+" - VST - "+j["name"])' "$project/plugin.json")
  tmp=$(mktemp -d)
  if remote --drive "$MPC_DRIVE" --yes export "$name" --out "$tmp" >/dev/null 2>&1 && ls "$tmp"/*.zip >/dev/null 2>&1; then
    mkdir -p "$project/Content"
    (cd "$tmp" && unzip -q -o ./*.zip "$name/Content/*" -d unpacked 2>/dev/null || true)
    if [ -d "$tmp/unpacked/$name/Content" ]; then
      # -n: keep the local file when both exist
      cp -R -n "$tmp/unpacked/$name/Content/." "$project/Content/" 2>/dev/null || true
      echo "Kept the device's Content/ ($(find "$tmp/unpacked/$name/Content" -type f | wc -l | tr -d ' ') files on the device)"
    fi
  fi
  rm -rf "$tmp"
fi

# build.sh stops on a build, validation or host_sim failure, so a broken build is never sent.
[ "$build" = yes ] && ./build.sh "$project"

if [ -n "$content" ]; then
  # A copy of the built plugin folder with the library as its Content/, and in it whatever the
  # Save button has written on the device.
  [ -d "$content/Content" ] || { echo "no $content/Content" >&2; exit 1; }
  name=$(python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print(j["manufacturer"]+" - VST - "+j["name"])' "$project/plugin.json")
  stage=$here/build/stage
  rm -rf "$stage"
  mkdir -p "$stage"
  cp -R "$project/build/$name" "$stage/$name"
  rm -rf "$stage/$name/Content"
  cp -R "$content/Content" "$stage/$name/Content"
  tmp=$(mktemp -d)
  if remote --drive "$MPC_DRIVE" --yes export "$name" --out "$tmp" >/dev/null 2>&1 && ls "$tmp"/*.zip >/dev/null 2>&1; then
    (cd "$tmp" && unzip -q -o ./*.zip "$name/Content/SYNTHS/User/*" -d unpacked 2>/dev/null || true)
    if [ -d "$tmp/unpacked/$name/Content/SYNTHS/User" ]; then
      mkdir -p "$stage/$name/Content/SYNTHS/User" "$project/Content/SYNTHS/User"
      cp -R -n "$tmp/unpacked/$name/Content/SYNTHS/User/." "$stage/$name/Content/SYNTHS/User/" 2>/dev/null || true
      cp -R -n "$tmp/unpacked/$name/Content/SYNTHS/User/." "$project/Content/SYNTHS/User/" 2>/dev/null || true
      echo "Kept $(find "$tmp/unpacked/$name/Content/SYNTHS/User" -type f | wc -l | tr -d ' ') presets saved on the device (a copy is in $project/Content/SYNTHS/User)"
    fi
  fi
  rm -rf "$tmp"
  # And the ones earlier deploys brought back into the project: after the plugin changes its
  # name its old folder is not looked in, and they would otherwise stay behind.
  if [ -d "$project/Content/SYNTHS/User" ]; then
    mkdir -p "$stage/$name/Content/SYNTHS/User"
    cp -R -n "$project/Content/SYNTHS/User/." "$stage/$name/Content/SYNTHS/User/" 2>/dev/null || true
  fi
  # Jamie's own device keeps diagnostics.txt fresh while it plays (CLAUDE.md, "Seeing inside the
  # device"); a release does that only where this file has been put.
  : > "$stage/$name/diagnostics-on"
  remote --drive "$MPC_DRIVE" --yes add "$stage/$name"
else
  remote --drive "$MPC_DRIVE" --yes update "$project"
fi

if [ "$restart" = yes ]; then
  remote --yes restart
else
  echo "Installed. The MPC will not load the new build until its app restarts:"
  echo "  ./deploy.sh $project --no-build --restart     (unsaved project data on the MPC is lost)"
fi
