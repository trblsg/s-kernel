#!/bin/bash
# verify-boot.sh SERIAL BOOT_IMG LABEL [WAIT_MIN=12] : ONE verification boot of the phone SERIAL (in TWRP, lock held by the caller).
# Flashes BOOT (serial-pinned, read-back verified), reboots, captures the USB console with epoch timestamps into work/boot-LABEL.log,
# after WAIT_MIN minutes prints tools/boot-metrics.py and ends the run with 'return' (-> TWRP), unless KEEP=1: then the capture is stopped,
# the console device is left to the caller (usb-shot.py / "touch X Y" / "button home" / "return") and its name is written to work/boot-LABEL.dev.
# Does NOT touch USERDATA.
set -uo pipefail
SERIAL=${1:?SERIAL}; BOOT=${2:?BOOT_IMG}; LABEL=${3:?LABEL}; WAIT=${4:-12}
H=$(cd "$(dirname "$0")" && pwd); ADB=${ADB:-adb}; A="$ADB -s $SERIAL"
LOG=$H/work/boot-$LABEL.log
[[ $($A get-state 2>/dev/null) == recovery ]] || { echo "$SERIAL not in TWRP"; exit 1; }
LOC=$(python3 -I "$H/usb-console.py" serial "$SERIAL" | head -1); [[ -n $LOC ]] || { echo "no USB location for $SERIAL"; exit 1; }   # physical port: stays the same after the reboot
before=$(ls /dev/cu.usbmodem* 2>/dev/null | tr '\n' ' ')
N=$(stat -f %z "$BOOT"); L=$(shasum -a 256 "$BOOT" | cut -c1-64)
$A push "$BOOT" /tmp/boot.img >/dev/null && [[ $($A shell sha256sum /tmp/boot.img | cut -c1-64) == "$L" ]] || { echo "push mismatch"; exit 1; }
$A shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 2>/dev/null; sync"
[[ $($A shell "head -c $N /dev/block/by-name/BOOT | sha256sum" | cut -c1-64) == "$L" ]] || { echo "BOOT read-back mismatch"; exit 1; }
echo "FLASH-VERIFIED $L ($SERIAL) $(date -u +%T)"
rm -f "${LOG%.log}.dev"; : > "$LOG"
"$H/console-capture.sh" "$LOC" "$LOG" &     # started BEFORE the reboot: nothing of the boot is lost
CAT=$!
$A reboot; T0=$(date +%s); echo "rebooted $(date -u +%T); port $LOC"
for i in $(seq 1 120); do [[ -s ${LOG%.log}.dev ]] && break; sleep 1; done
[[ -s ${LOG%.log}.dev ]] && echo "console $(cat "${LOG%.log}.dev") up after $(( $(date +%s) - T0 )) s" || echo "WARNING: no console device within 120 s (still capturing)"
sleep $((WAIT*60))
DEV=$(cat "${LOG%.log}.dev" 2>/dev/null)
if [[ ${KEEP:-0} == 1 ]]; then pkill -P $CAT 2>/dev/null; kill $CAT 2>/dev/null; else printf 'return\n' > "$DEV"; sleep 8; pkill -P $CAT 2>/dev/null; kill $CAT 2>/dev/null; fi
echo "run ended $(date -u +%T), boot started at epoch $T0"; python3 -I "$H/boot-metrics.py" "$LOG"
