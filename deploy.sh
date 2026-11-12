#!/bin/sh
# Build a plugin, test it in the host simulator, and install it on an MPC over the network with
# the SDK's mpc_remote.py (the MPC's developer mode must be on; see CLAUDE.md, "Deploying").
#
#   ./deploy.sh plugins/deluge-proto              build, test, install
#   ./deploy.sh plugins/deluge-proto --restart    ... then restart the MPC app so it rescans
#   ./deploy.sh --info | --doctor | --list        talk to the MPC without building
#   ./deploy.sh plugins/deluge-proto --no-build   install what is already in build/
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
[ -f "$here/.mpc.env" ] && . "$here/.mpc.env"
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
for arg in "$@"; do
  case $arg in
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
# build.sh stops on a build, validation or host_sim failure, so a broken build is never sent.
[ "$build" = yes ] && ./build.sh "$project"

remote --drive "$MPC_DRIVE" --yes update "$project"

if [ "$restart" = yes ]; then
  remote --yes restart
else
  echo "Installed. The MPC will not load the new build until its app restarts:"
  echo "  ./deploy.sh $project --no-build --restart     (unsaved project data on the MPC is lost)"
fi
