#!/bin/bash
# build-full-root.sh — iOS 27.0.1 (24A446, N104AP) IPSW images -> FULL, flashable root for the Galaxy S9 (starlte).   mini-side tool.
#
#   sudo ./build-full-root.sh build                 # IPSW -> out/root-full.img (raw HFSX, volume at byte 0; sparse file)
#   ./build-full-root.sh check MOUNTED_ROOT         # offline checks of a mounted root
#   sudo ./build-full-root.sh verify IMG            # fsck + attach read-only + offline checks of a built image
#   ./build-full-root.sh flash SERIAL IMG [BOOT]    # TWRP only; SERIAL is REQUIRED and pinned in every adb call
#
# What "full" means: every file of the OS image and of the SystemOS + AppOS cryptexes, nothing trimmed (no lproj/espresso/mlmodelc/
# TTS/LinguisticData/Health/... filtering, all stock apps installed into /Applications); only the two 1.2 GB dyld_shared_cache *.symbols
# files are left out. On top, the owner build's root edits, each one an idempotent step (tools/steps.d/NN-*.sh ROOT; peers drop new
# NN-*.sh files there, they run in name order):
#   cache (tools/pipeline/cache-pipeline.sh)  a64e + LSE + corecrypto PAC + PACGA/LDRA rewrites, Settings / kbd / QuartzCore-clamp /
#                                             PosterBoard Fix A cache patches, full AOT rewrite (rewrite-grow -> fix-cs -> grow-check)
#   10 macho-a64e   PAC rewrite of every arm64e Mach-O outside the caches
#   20 launchd      hardware + crash-loop daemons only (lists/), _PanicOnCrash dropped, launchctl overrides   [DISABLE_HOGS=1 = opt-in CPU-hog list]
#   30 overlay      sbca/m/b/s dylibs (brightness shim = b.dylib), libOSMesa/libsbgl, StarbiteMetal, cc_fips_test stand-in, wallpaper layers
#   40 prefs        Reduce Motion / Transparency OFF, CA compression off, watchdogs off
#   50 xpc-disable  MessagesBlastDoorService
# NOT applied (owner wants the real iOS 27): the 86 "speed" daemon trims, Spotlight/Siri/photos/ML/intelligence off-switches.
# Env: SIZE_GB=32  OUT=out/root-full.img  REUSE_CACHE=1 (use work/cache if its DONE marker exists)  DISABLE_HOGS=1  KEEP_MOUNTED=1
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
PIPE=${IOS27_PIPELINE:-$HERE/pipeline}; export IOS27_PIPELINE=$PIPE   # cache pipeline + stage script (public part; private pieces via IOS27_PRIVATE)
export IOS27_PRIVATE=${IOS27_PRIVATE:-$HERE/private}   # overlay/, patches.txt, oracle/ (Apple-derived; never in the repo)
cmd=${1:-}; shift || true
ADB=${ADB:-adb}
PY=${PY:-python3}; export PY
IPSW=${IPSW:-$HERE/ipsw}
OS_DMG=${OS_DMG:-$IPSW/043-70213-665.dmg}; SYS_DMG=${SYS_DMG:-$IPSW/043-69708-712.dmg}; APP_DMG=${APP_DMG:-$IPSW/043-69297-793.dmg}
BUILD_VERSION=${BUILD_VERSION:-24A446}
SIZE_GB=${SIZE_GB:-32}; OUT=${OUT:-$HERE/out/root-full.img}; W=$HERE/work
log() { echo "[$(date -u +%T)] $*"; }

detach_if() { [[ -n ${1:-} ]] && { umount "$1" 2>/dev/null || true; hdiutil detach "$1" >/dev/null 2>&1 || hdiutil detach -force "$1" >/dev/null 2>&1 || true; }; }
attach_src() { # attach_src DMG MOUNTPOINT : read-only, owners on, raw APFS image
    hdiutil attach -readonly -nobrowse -owners on -imagekey diskimage-class=CRawDiskImage -mountpoint "$2" "$1" >/dev/null
}

case $cmd in
build)
    [[ ${EUID} == 0 ]] || { echo "run as root: sudo $0 build" >&2; exit 2; }
    OWNER=${SUDO_USER:?run via sudo}
    for f in "$OS_DMG" "$SYS_DMG" "$APP_DMG"; do [[ -f $f ]] || { echo "missing $f" >&2; exit 1; }; done
    free_gb=$(df -g "$HERE" | awk 'NR==2 {print $4}'); need=${MIN_FREE_GB:-62}
    (( free_gb >= need )) || { echo "only $free_gb GB free, need >= $need (keeps 40 GB free after the ~22 GB root + 6 GB cache)" >&2; exit 1; }
    mkdir -p "$W" "$(dirname "$OUT")"; chown "$OWNER" "$W" "$(dirname "$OUT")"
    # ---- 1. cache (unprivileged) ----
    SYSMNT=$W/mnt-sys; OSMNT=$W/mnt-os; APPMNT=$W/mnt-app; ROOTMNT=$W/mnt-root
    mkdir -p "$SYSMNT" "$OSMNT" "$APPMNT" "$ROOTMNT"
    for m in "$ROOTMNT" "$OSMNT" "$SYSMNT" "$APPMNT"; do detach_if "$m"; done
    attach_src "$OS_DMG" "$OSMNT"; attach_src "$SYS_DMG" "$SYSMNT"; attach_src "$APP_DMG" "$APPMNT"
    cleanup() { detach_if "${ROOTMNT:-}"; [[ -n ${RDEV:-} ]] && hdiutil detach "$RDEV" >/dev/null 2>&1 || true; detach_if "$OSMNT"; detach_if "$SYSMNT"; detach_if "$APPMNT"; }
    trap cleanup EXIT
    if [[ ${REUSE_CACHE:-0} == 1 && -f $W/cache/DONE ]]; then log "reusing $W/cache"; else
        log "cache pipeline (a64e, LSE, corecrypto, PACGA, patches, full AOT rewrite)"
        sudo -u "$OWNER" -H env PY="$PY" "$PIPE/cache-pipeline.sh" "$SYSMNT/System/Library/Caches/com.apple.dyld" "$W/cache" 2>&1 | tee "$W/cache-pipeline.log"
        grep -q CACHE-PIPELINE-DONE "$W/cache-pipeline.log" || { echo "cache pipeline failed" >&2; exit 1; }
        sudo -u "$OWNER" touch "$W/cache/DONE"
    fi
    # ---- 2. blank HFSX volume, byte 0 of USERDATA ----
    log "creating ${SIZE_GB} GiB sparse raw image $OUT"
    rm -f "$OUT"; mkfile -n "${SIZE_GB}g" "$OUT"
    RDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$OUT" | awk 'NR==1 {print $1}')
    newfs_hfs -s -J -b 4096 -v IOSROOT "/dev/r${RDEV#/dev/}" >/dev/null
    mount -t hfs -o owners "$RDEV" "$ROOTMNT"
    # ---- 3. stage: full OS + cryptexes, all apps, rewritten cache ----
    log "staging (this copies ~22 GB)"
    CACHE_SRC=$W/cache/c STAGE_ALL_APPS=1 INSTALL_STAGED_APPS=1 bash "$PIPE/root/stage-full-root.sh" "$OSMNT" "$SYSMNT" "$APPMNT" "$ROOTMNT"
    log "herolte tune (setup done, CommCenter out, autorun job, containers skeleton)"
    bash "$IOS27_PRIVATE/herolte-tune-ios-root.sh" "$ROOTMNT" "$BUILD_VERSION"
    n=0; for app in "$ROOTMNT"/private/var/staged_system_apps/*.app; do [[ -d $app && -d $ROOTMNT/Applications/$(basename "$app") ]] || continue; rm -rf "$app"; n=$((n+1)); done
    echo "staged_system_apps: removed $n copies already in /Applications"
    # ---- 4. steps ----
    for s in "$HERE"/tools/steps.d/[0-9][0-9]-*.sh; do log "step $(basename "$s")"; bash "$s" "$ROOTMNT"; done
    sync
    log "offline checks"; chk=0; bash "$HERE/check-root.sh" "$ROOTMNT" | tee "$W/check-root.log" || true; grep -q "ALL HARD CHECKS PASSED" "$W/check-root.log" || chk=1
    df -h "$ROOTMNT"; sync
    umount "$ROOTMNT"; log "fsck_hfs"; fsck_hfs -n -f "/dev/r${RDEV#/dev/}" 2>&1 | tail -4 | tee -a "$W/check-root.log"
    hdiutil detach "$RDEV" >/dev/null; RDEV=
    chown "$OWNER" "$OUT"; chmod 644 "$OUT"; ls -ls "$OUT"; shasum -a 256 "$OUT" | tee "$OUT.sha256"
    log "BUILD-DONE $OUT (offline checks: $([[ $chk == 0 ]] && echo passed || echo FAILED))"; exit $chk
    ;;
verify)
    IMG=${1:?usage: verify IMG}; [[ ${EUID} == 0 ]] || { echo "run as root: sudo $0 verify IMG" >&2; exit 2; }
    mkdir -p "$W/mnt-verify"; VDEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" | awk 'NR==1 {print $1}')
    trap 'umount "$W/mnt-verify" 2>/dev/null; hdiutil detach "$VDEV" >/dev/null 2>&1' EXIT
    fsck_hfs -n -f "/dev/r${VDEV#/dev/}" 2>&1 | tail -3
    mount -t hfs -o owners,rdonly "$VDEV" "$W/mnt-verify"; bash "$HERE/check-root.sh" "$W/mnt-verify" | tee "$W/check-verify.log"
    ;;
check)
    exec bash "$HERE/check-root.sh" "${1:?mounted root}"
    ;;
flash)
    SERIAL=${1:?usage: flash SERIAL IMG [BOOT]}; IMG=${2:?IMG}; BOOT=${3:-}
    export ANDROID_SERIAL=$SERIAL
    A="$ADB -s $SERIAL"
    [[ $($A get-state 2>/dev/null) == recovery ]] || { echo "$SERIAL is not in TWRP" >&2; exit 1; }
    [[ $($A get-serialno) == "$SERIAL" ]] || { echo "wrong serial" >&2; exit 1; }
    pin=$(mktemp /tmp/adbpin.XXXX); printf '#!/bin/sh\ncase "$1" in kill-server|start-server) exit 0;; esac\nexec %s -s %s "$@"\n' "$ADB" "$SERIAL" > "$pin"; chmod +x "$pin"
    python3 "${FASTFLASH:-$HERE/fastflash-root.py}" "$pin" "$IMG" "${IMG}.flash-$SERIAL.state"
    if [[ -n $BOOT ]]; then
        N=$(stat -f %z "$BOOT"); L=$(shasum -a 256 "$BOOT" | cut -c1-64)
        $A push "$BOOT" /tmp/boot.img >/dev/null; [[ $($A shell sha256sum /tmp/boot.img | cut -c1-64) == "$L" ]] || { echo "push mismatch" >&2; exit 1; }
        $A shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 2>/dev/null; sync"
        [[ $($A shell "head -c $N /dev/block/by-name/BOOT | sha256sum" | cut -c1-64) == "$L" ]] || { echo "BOOT read-back mismatch" >&2; exit 1; }
        echo "FLASH-VERIFIED $L"
    fi
    rm -f "$pin"; echo "flashed (not rebooted): $A reboot"
    ;;
*) sed -n 2,22p "$0"; exit 2;;
esac
