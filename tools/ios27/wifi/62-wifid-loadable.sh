#!/bin/bash
# 62-wifid-loadable.sh ROOT : let launchd start iOS's Wi-Fi daemon on the S9. Idempotent; a root-build step (needs root, ROOT mounted
# with owners on). OPT-IN (WIFI_WIFID=1) until a phone run shows wifid is harmless on every kernel of the release line.
#
# iOS 27's com.apple.wifid job carries `_LimitLoadToDeviceTree` = (wlan, marconi-wifi, centauri-alpha, ...): launchd loads it
# only where the device tree has such a node, and the S9's ADT has none, so wifid never starts and Settings shows Wi-Fi off
# even with the radio joined and pinging in the kernel (phone 2, 10 Nov 2026). Scion's 16.5 build drops the key from the job
# (userland/mkjobs.py); this does the same in the launchd cache /System/Library/xpc/launchd.plist (and in
# launchd-disabled.plist, should the job have been moved there), and in the job's own plist under LaunchDaemons.
# The other way, without touching the root, is a test image whose ADT has /arm-io/wlan (adt-wlan-node.py).
set -euo pipefail
R=$1
[[ ${WIFI_WIFID:-0} == 1 ]] || { echo "62-wifid-loadable: skipped (WIFI_WIFID=1 enables it)"; exit 0; }
[[ -f $R/System/Library/xpc/launchd.plist ]] || { echo "62-wifid-loadable: $R has no launchd.plist" >&2; exit 1; }
/usr/bin/python3 - "$R" <<'PY'
import os, plistlib, sys
root = sys.argv[1]
changed = 0
for rel in ('System/Library/xpc/launchd.plist', 'System/Library/xpc/launchd-disabled.plist'):
    p = os.path.join(root, rel)
    if not os.path.exists(p):
        continue
    d = plistlib.load(open(p, 'rb'))
    jobs = d.get('LaunchDaemons', d) if rel.endswith('launchd.plist') else d
    n = 0
    for k, j in jobs.items():
        if isinstance(j, dict) and j.get('Label') == 'com.apple.wifid' and '_LimitLoadToDeviceTree' in j:
            del j['_LimitLoadToDeviceTree']
            n += 1
    if n:
        st = os.stat(p)
        with open(p, 'r+b') as f:
            f.seek(0); f.write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); f.truncate()
        os.chown(p, st.st_uid, st.st_gid)
        changed += n
job = os.path.join(root, 'System/Library/LaunchDaemons/com.apple.wifid.plist')
if os.path.exists(job):
    j = plistlib.load(open(job, 'rb'))
    if '_LimitLoadToDeviceTree' in j:
        del j['_LimitLoadToDeviceTree']
        st = os.stat(job)
        with open(job, 'wb') as f:
            f.write(plistlib.dumps(j, fmt=plistlib.FMT_BINARY))
        os.chown(job, st.st_uid, st.st_gid)
        changed += 1
print('62-wifid-loadable: com.apple.wifid no longer limited to a device-tree node (%d place(s) changed)' % changed)
PY
