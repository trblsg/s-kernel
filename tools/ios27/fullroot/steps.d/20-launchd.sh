#!/bin/bash
# 20-launchd.sh ROOT : daemon edits (see edit-launchd.py + lists/). DISABLE_HOGS=1 adds lists/daemons-hogs.list (off by default).
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd)
/usr/bin/python3 "$H/edit-launchd.py" "$R" "$H/../lists" ${DISABLE_HOGS:+--hogs}
