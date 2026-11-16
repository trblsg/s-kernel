#!/bin/bash
# uninstall-dptest.sh MOUNTPOINT : removes binary, plists, launchd cache entries (logs left in place)
set -euo pipefail
M=${1:?usage: uninstall-dptest.sh MOUNTPOINT}
rm -f "$M/usr/local/bin/dptest" "$M/System/Library/LaunchDaemons/com.scion.dptest.plist" "$M/System/Library/LaunchDaemons/com.scion.dptest.mobile.plist" "$M/System/Library/LaunchDaemons/com.scion.dptest.passcode.plist"
/usr/bin/python3 - "$M" <<'PY'
import plistlib, sys, os
lp = sys.argv[1] + '/System/Library/xpc/launchd.plist'
if os.path.exists(lp):
    d = plistlib.load(open(lp, 'rb'))
    for n in ('com.scion.dptest', 'com.scion.dptest.mobile', 'com.scion.dptest.passcode'): d['LaunchDaemons'].pop('/System/Library/LaunchDaemons/%s.plist' % n, None)
    with open(lp, 'r+b') as f:
        f.seek(0); f.write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); f.truncate()
PY
echo "uninstalled from $M"
