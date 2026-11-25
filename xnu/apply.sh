#!/bin/bash
# Get the kernel tree the OS profile names: Scion-Project/xnu, the fork of Apple's XNU, at the commit the profile pins, with Scion's
# changes as commits on Apple's release tag (docs/xnu-fork.md); then add what is not in the fork: the drivers (drivers/map, and the
# SoC and device repositories checked out) and the drivers' configuration (tools/hakios-config.py kernel).
# usage: xnu/apply.sh [DIR]              (default: $XNU_SRC, else the OS profile's tree: xnu-src/<xnu_tag> next to this directory)
# A profile on another XNU base than xnu-8796 is made by xnu/apply-series.sh, which this calls.
# env:   XNU_FORK_URL  where to fetch the fork from (default the profile's xnu_fork; a clone that has the commit will do:
#                      XNU_FORK_URL=file:///path/to/xnu)
# The result is a git tree on the branch "hakios", the fork's commit checked out; the drivers and the configuration are not committed.
# A directory that already exists is left alone.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python3 "$HERE/../tools/os-profile.py" require || exit 1
[ "$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get base)" = xnu-8796 ] || exec "$HERE/apply-series.sh" "$@"
DIR=${1:-${XNU_SRC:-$HERE/../$(OS_PROFILE_QUIET=1 python3 "$HERE/../tools/os-profile.py" get src)}}

. "$HERE/fork.sh"
[ -e "$DIR" ] && { echo "apply.sh: $DIR already exists; not touching it" >&2; exit 1; }
[ -f "$HERE/../drivers/map" ] || { echo "apply.sh: no drivers/map next to xnu/ (the drivers are in drivers/)" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")
trap 'echo "apply.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
# Scion-Project/xnu at the profile's commit: Apple's tag with Scion's changes, HFS Plus and the numbered patches as commits
fork_checkout "$DIR"
# the drivers are kept by subsystem (drivers/<subsystem>/), not at their place in Apple's tree: drivers/map says where
# each one goes, one line per file: "<subsystem> <path in the tree>"
NDRIVERS=0
while read -r sub tree _; do
	case "$sub" in ''|'#'*) continue ;; esac
	[ -f "$HERE/../drivers/$sub/$(basename "$tree")" ] || { echo "apply.sh: drivers/$sub/$(basename "$tree") is in drivers/map but missing" >&2; false; }
	mkdir -p "$(dirname "$tree")"
	cp -p "$HERE/../drivers/$sub/$(basename "$tree")" "$tree"
	NDRIVERS=$((NDRIVERS + 1))
done < "$HERE/../drivers/map"
if [ "$(_scion_profile_get fork_copy || true)" = profiles/ios-16.5.copy ]; then
	python3 "$HERE/p125-timer-observer.py" --verify "$DIR" || exit 1
fi
# the drivers of the SoC and device repositories checked out (socs/*/drivers/, devices/*/drivers/)
python3 "$HERE/../tools/hakios-config.py" external-files | while IFS="$(printf '\t')" read -r src tree; do
	mkdir -p "$(dirname "$tree")"
	cp -p "$src" "$tree"
done
# which of them are built is the configuration's (drivers/*/conf, CONFIG_HAKIOS_*): put into the tree as XNU reads it.
# What that writes is made again for every build, so it is kept out of git status.
printf '%s\n' config/MASTER.hakios bsd/dev/hakios.drivers.h >> .git/info/exclude
python3 "$HERE/../tools/hakios-config.py" kernel --tree "$DIR"
echo "apply.sh: $DIR: Scion-Project/xnu ${FORK_COMMIT:0:12} + $NDRIVERS drivers"
