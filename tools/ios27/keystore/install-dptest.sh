#!/bin/bash
# install-dptest.sh MOUNTPOINT : idempotent. Run with sudo on a root image attached with `hdiutil attach -owners on`.
set -euo pipefail
M=${1:?usage: install-dptest.sh MOUNTPOINT}; H=$(cd "$(dirname "$0")" && pwd)
[ -d "$M/System/Library/LaunchDaemons" ] || { echo "not an iOS root: $M"; exit 1; }
BIN=${DPTEST_BIN:-$H/dptest.arm64}   # DPTEST_BIN=$H/dptest.arm64e to try the PAC build
install -d -m 0755 "$M/usr/local/bin"
cp "$BIN" "$M/usr/local/bin/dptest"; chmod 0755 "$M/usr/local/bin/dptest"
for p in com.scion.dptest.plist com.scion.dptest.mobile.plist; do
  cp "$H/$p" "$M/System/Library/LaunchDaemons/$p"; chmod 0644 "$M/System/Library/LaunchDaemons/$p"
done
# passcode job: only with DPTEST_PASSCODE=1, otherwise removed (file here, cache entry in the python block)
PCP="$M/System/Library/LaunchDaemons/com.scion.dptest.passcode.plist"
if [ "${DPTEST_PASSCODE:-0}" = 1 ]; then
  cp "$H/com.scion.dptest.passcode.plist" "$PCP"; chmod 0644 "$PCP"
  [ "$(id -u)" = 0 ] && chown 0:0 "$PCP"
else rm -f "$PCP"; fi
mkdir -p "$M/private/var/local/oslog"; chmod 0777 "$M/private/var/local/oslog"
if [ "$(id -u)" = 0 ]; then
  chown 0:0 "$M/usr/local/bin/dptest" "$M/System/Library/LaunchDaemons/com.scion.dptest.plist" "$M/System/Library/LaunchDaemons/com.scion.dptest.mobile.plist"
  chown 0:0 "$M/usr/local/bin" 2>/dev/null || true
else echo "WARNING: not root, ownership not set"; fi
# this root loads jobs from the combined cache System/Library/xpc/launchd.plist (key = plist path), like install-metal27.py
DPTEST_PASSCODE=${DPTEST_PASSCODE:-0} /usr/bin/python3 - "$M" "$H" <<'PY'
import plistlib, sys, os, shutil
M, H = sys.argv[1:3]
lp = M + '/System/Library/xpc/launchd.plist'
if os.path.exists(lp):
    if not os.path.exists(lp + '.before-dptest'): shutil.copy2(lp, lp + '.before-dptest')
    d = plistlib.load(open(lp, 'rb'))
    for n in ('com.scion.dptest', 'com.scion.dptest.mobile'):
        k = '/System/Library/LaunchDaemons/%s.plist' % n
        d['LaunchDaemons'][k] = plistlib.load(open('%s/%s.plist' % (H, n), 'rb'))
    pk = '/System/Library/LaunchDaemons/com.scion.dptest.passcode.plist'
    if os.environ.get('DPTEST_PASSCODE') == '1':
        d['LaunchDaemons'][pk] = plistlib.load(open(H + '/com.scion.dptest.passcode.plist', 'rb'))
    else:
        d['LaunchDaemons'].pop(pk, None)
    with open(lp, 'r+b') as f:
        f.seek(0); f.write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); f.truncate()
    print('launchd.plist: jobs added/updated (%d jobs total)' % len(d['LaunchDaemons']))
else:
    print('no xpc/launchd.plist cache; relying on LaunchDaemons dir only')
PY
echo "installed: $M/usr/local/bin/dptest ($(basename "$BIN")), 2 plists (+passcode job: ${DPTEST_PASSCODE:-0}), oslog dir. Results: /private/var/local/oslog/dptest.log, dptest-mobile.log"
