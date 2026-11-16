#!/bin/bash
# 20-launchd.sh ROOT : daemon edits (see edit-launchd.py + lists/). DAEMON_SET=bp (default): exactly the owner build's job set;
# DAEMON_SET=min: hardware + measured crash loops only (the real iOS 27, everything else on); DISABLE_HOGS=1 adds the CPU-hog list to min.
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd)
/usr/bin/python3 "$H/edit-launchd.py" "$R" "$H/../lists" --set "${DAEMON_SET:-bp}" ${DISABLE_HOGS:+--hogs}
