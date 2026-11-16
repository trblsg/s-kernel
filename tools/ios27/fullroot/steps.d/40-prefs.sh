#!/bin/bash
# 40-prefs.sh ROOT : preferences (see edit-prefs.py)
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd)
/usr/bin/python3 "$H/edit-prefs.py" "$R"
