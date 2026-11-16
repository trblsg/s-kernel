#!/bin/bash
# 80-metal-ca.sh ROOT : CoreAnimation's Metal renderer in backboardd (real Liquid Glass) as the default compositor.
#   ENABLE_METAL_CA=1 (default):
#     - installs this build of StarbiteMetal.bundle (PanVK's kernel shim asks for asynchronous submits and answers timeline
#       queries truthfully; the frontend has the SWEET_MTL2_FLUSH cache-maintenance policy)
#     - /private/var/local/mtl2-env: SWEET_MTL2_DEXR=1, SWEET_MTL2_FLUSH=2, STARBITE_DRM_LEAKS=0 (other lines kept)
#     - /private/var/local/mtl2-clients = exactly $METAL_CA_CLIENTS (default "backboardd"): the list is the gate, a process
#       that is not on it gets no Metal device. Every Metal client wires >= 57 MiB of GPU memory when its device is made, so the
#       poster renderers are NOT on it by default; to give ONE of them Metal: METAL_CA_CLIENTS="backboardd CollectionsPoster".
#     - the test jobs com.starbite.m2vtest / com.starbite.mtl27test out of /System/Library/xpc/launchd.plist (m2vtest started
#       a Vulkan client at every boot); the tools stay in /usr/local/bin for use by hand
#     - the pipeline cache directory /private/var/local/m2v-cache (0777) and /private/var/local/mtl2.log (0666)
#   ENABLE_METAL_CA=0: rollback to the OpenGL ES compositor: backboardd out of mtl2-clients (the bundle stays; without
#     backboardd listed and without sbgl=0 nothing in backboardd uses it).
# The renderer is chosen on the kernel command line: sbgl=0 (Metal) or no sbgl argument (OpenGL ES, sbca). BOTH must agree:
#   Metal-CA = this step with ENABLE_METAL_CA=1 + "sbgl=0 sbmtl=1" on the line; GL = ENABLE_METAL_CA=0 + no sbgl=0.
# Idempotent; works on a root mounted with owners (as root: files become root:wheel) or without (-owners off: in-place writes).
# ios27-fable-hard, 9 Nov 2026. Files: ./files/StarbiteMetal.bundle (MANIFEST.sha256). See README.md.
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd); F=${METAL_CA_FILES:-$H/files}   # the bundle you built (never in git): tools/ios27/s9/prepare-root-inputs.sh
ON=${ENABLE_METAL_CA:-1}
CLIENTS=${METAL_CA_CLIENTS:-backboardd}
L=$R/private/var/local; B=$R/System/Library/Extensions/StarbiteMetal.bundle
if [[ $ON == 1 ]]; then
    [[ -f $F/MANIFEST.sha256 ]] || { echo "80-metal-ca: no StarbiteMetal.bundle in $F: build it (docs/ios27/METAL.md, docs/ios27/INSTALL-S9.md) or run with ENABLE_METAL_CA=0 (OpenGL ES compositor)" >&2; exit 1; }
    (cd "$F" && shasum -a 256 -c MANIFEST.sha256 >/dev/null) || { echo "80-metal-ca: $F/MANIFEST.sha256 mismatch" >&2; exit 1; }
fi
put() {  # put SRC DST MODE : in place when DST exists (owner, flags and xattrs stay), else a new root:wheel file
    if [[ -f $2 ]]; then cmp -s "$1" "$2" || cat "$1" > "$2"; else mkdir -p "$(dirname "$2")"; cat "$1" > "$2"; chown 0:0 "$2" 2>/dev/null || true; fi
    chmod "$3" "$2"
}
same() { [[ -f $2 ]] && [[ "$(cat "$2")" == "$1" ]] || printf '%s\n' "$1" > "$2"; }   # same TEXT FILE : write only on a change
mkdir -p "$L"
touch "$L/mtl2-clients" "$L/mtl2-env"
if [[ $ON == 1 ]]; then
    put "$F/StarbiteMetal.bundle/StarbiteMetal" "$B/StarbiteMetal" 755
    put "$F/StarbiteMetal.bundle/libvulkan_panfrost.dylib" "$B/libvulkan_panfrost.dylib" 755
    put "$F/StarbiteMetal.bundle/Info.plist" "$B/Info.plist" 644
    put "$F/StarbiteMetal.bundle/_CodeSignature/CodeResources" "$B/_CodeSignature/CodeResources" 644
    chown -R 0:0 "$B" 2>/dev/null || true
    # mtl2-env: these three lines, once each, whatever else is there
    T=$(mktemp); grep -v '^SWEET_MTL2_DEXR=\|^SWEET_MTL2_FLUSH=\|^STARBITE_DRM_LEAKS=' "$L/mtl2-env" > "$T" || true
    printf 'SWEET_MTL2_DEXR=1\nSWEET_MTL2_FLUSH=%s\nSTARBITE_DRM_LEAKS=0\n' "${METAL_CA_FLUSH:-2}" >> "$T"
    cmp -s "$T" "$L/mtl2-env" || cat "$T" > "$L/mtl2-env"; rm -f "$T"
    case " $CLIENTS " in *" backboardd "*) ;; *) CLIENTS="backboardd $CLIENTS";; esac
    same "$CLIENTS" "$L/mtl2-clients"
    install -d -m 0777 "$L/m2v-cache" 2>/dev/null || mkdir -p "$L/m2v-cache"; chmod 0777 "$L/m2v-cache"
    [[ -f $L/mtl2.log ]] || : > "$L/mtl2.log"; chmod 0666 "$L/mtl2.log"
    rm -f "$L/mtl2-telemetry"                      # (the 10 s telemetry is a debugging aid: touch it to get it back)
    # the two test jobs out of launchd's cache (and their plists, if any)
    rm -f "$R/System/Library/LaunchDaemons/com.starbite.m2vtest.plist" "$R/System/Library/LaunchDaemons/com.starbite.mtl27test.plist"
    JOBS=$(/usr/bin/python3 - "$R/System/Library/xpc/launchd.plist" <<'PY'
import plistlib, sys, os
lp = sys.argv[1]; gone = 0
if os.path.exists(lp):
    raw = open(lp, 'rb').read(); d = plistlib.loads(raw)
    for n in ('com.starbite.m2vtest', 'com.starbite.mtl27test'):
        gone += d.get('LaunchDaemons', {}).pop('/System/Library/LaunchDaemons/%s.plist' % n, None) is not None
    if gone:
        new = plistlib.dumps(d, fmt=plistlib.FMT_BINARY if raw[:6] == b'bplist' else plistlib.FMT_XML)
        with open(lp, 'r+b') as f:        # in place: the file keeps its owner, mode and flags
            f.seek(0); f.write(new); f.truncate()
print(gone)
PY
)
    echo "80-metal-ca: ON  clients: $(cat "$L/mtl2-clients") | env: $(tr '\n' ' ' < "$L/mtl2-env")| bundle $(shasum -a 256 "$B/StarbiteMetal" | cut -c1-12) | test jobs removed now: $JOBS"
else
    N=$(tr -s ' \n' '\n' < "$L/mtl2-clients" | grep -vx backboardd | grep -v '^$' | tr '\n' ' ' | sed 's/ $//' || true)
    same "$N" "$L/mtl2-clients"
    echo "80-metal-ca: OFF (OpenGL ES compositor)  clients: $(cat "$L/mtl2-clients")"
fi
chown 0:0 "$L/mtl2-clients" "$L/mtl2-env" 2>/dev/null || true; chmod 644 "$L/mtl2-clients" "$L/mtl2-env"
