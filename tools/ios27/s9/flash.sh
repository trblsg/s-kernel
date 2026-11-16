#!/bin/bash
# SPDX-License-Identifier: MIT-0
# flash.sh : write what you built to a Galaxy S9 (SM-G960F) that is in TWRP, over adb. The phone's serial is REQUIRED and is given
# to every adb call, so another phone on the same Mac is never touched. Only two partitions are ever written: BOOT (the image
# make-boot-image.sh made) and USERDATA (the root image; THIS ERASES EVERYTHING ANDROID KEPT IN /data). RECOVERY (TWRP), EFS and the
# boot loader are never written: TWRP stays your way back.
#   flash.sh SERIAL pull-template OUT.img     copy the phone's RECOVERY partition (TWRP) to OUT.img: the --template of make-boot-image.sh
#   flash.sh SERIAL backup-boot OUT.img       copy the phone's BOOT partition to OUT.img (keep it: it is how Android boots)
#   flash.sh SERIAL boot IMG [reboot]         write IMG to BOOT, read it back, compare SHA-256; `reboot` then starts it
#   flash.sh SERIAL root IMG                  write the raw root image to USERDATA in 64 MiB pieces, each read back and compared;
#                                             an interrupted run resumes (state file beside IMG). Asks for the word ERASE first
#                                             (ERASE=yes skips the question).
#   flash.sh SERIAL info                      model, recovery state, sizes of BOOT and USERDATA
# ADB=<adb> if it is not on PATH (Android platform-tools).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ADB=${ADB:-adb}
SERIAL=${1:?usage: flash.sh SERIAL pull-template|backup-boot|boot|root|info ...}; CMD=${2:?command}; shift 2
die() { echo "flash: $*" >&2; exit 1; }
command -v "$ADB" >/dev/null || die "no adb ($ADB): install Android platform-tools, or ADB=<path>"
A=("$ADB" -s "$SERIAL")
[ "$("${A[@]}" get-state 2>/dev/null)" = recovery ] || die "$SERIAL is not in recovery (TWRP): boot it with Volume Up + Bixby + Power"
model=$("${A[@]}" shell getprop ro.product.device | tr -d '\r')
case "$model" in starlte*) ;; *) die "this is '$model', not a Galaxy S9 (starlte): refusing" ;; esac
part() { "${A[@]}" shell "readlink -f /dev/block/by-name/$1" | tr -d '\r'; }
size() { "${A[@]}" shell "blockdev --getsize64 /dev/block/by-name/$1" | tr -d '\r'; }
pull() { # pull PARTITION OUT : copy a partition to the Mac and compare SHA-256
	local n; n=$(size "$1")
	"${A[@]}" exec-out "dd if=/dev/block/by-name/$1 bs=1048576 2>/dev/null" > "$2"
	[ "$(stat -f %z "$2")" = "$n" ] || die "$1: got $(stat -f %z "$2") bytes of $n"
	[ "$("${A[@]}" shell "sha256sum /dev/block/by-name/$1" | cut -c1-64)" = "$(shasum -a 256 "$2" | cut -c1-64)" ] || die "$1: the copy differs from the partition"
	echo "$1 ($n bytes) -> $2, SHA-256 checked"
}
case "$CMD" in
info)
	echo "device $model, serial $SERIAL, BOOT $(size BOOT) bytes at $(part BOOT), USERDATA $(size USERDATA) bytes at $(part USERDATA)" ;;
pull-template) pull RECOVERY "${1:?OUT.img}" ;;
backup-boot) pull BOOT "${1:?OUT.img}" ;;
boot)
	IMG=${1:?IMG}; [ -f "$IMG" ] || die "no $IMG"
	n=$(stat -f %z "$IMG"); [ "$n" -le "$(size BOOT)" ] || die "$IMG ($n bytes) is larger than BOOT"
	sum=$(shasum -a 256 "$IMG" | cut -c1-64)
	"${A[@]}" push "$IMG" /tmp/boot.img >/dev/null
	[ "$("${A[@]}" shell sha256sum /tmp/boot.img | cut -c1-64)" = "$sum" ] || die "the pushed copy differs"
	"${A[@]}" shell "dd if=/tmp/boot.img of=/dev/block/by-name/BOOT bs=1048576 2>/dev/null; sync; rm -f /tmp/boot.img"
	[ "$("${A[@]}" shell "head -c $n /dev/block/by-name/BOOT | sha256sum" | cut -c1-64)" = "$sum" ] || die "BOOT read back differs from $IMG"
	echo "BOOT written and read back: $sum"
	if [ "${2:-}" = reboot ]; then "${A[@]}" reboot; echo "rebooting"; else echo "not rebooted: $ADB -s $SERIAL reboot"; fi ;;
root)
	IMG=${1:?IMG}; [ -f "$IMG" ] || die "no $IMG"
	n=$(stat -f %z "$IMG"); [ "$n" -le "$(size USERDATA)" ] || die "$IMG ($n bytes) is larger than USERDATA ($(size USERDATA))"
	if [ "${ERASE:-}" != yes ]; then
		printf 'This overwrites USERDATA of %s (%s): everything Android stored on the phone is lost. Type ERASE to go on: ' "$SERIAL" "$model"
		read -r answer; [ "$answer" = ERASE ] || die "not confirmed"
	fi
	pin=$(mktemp "${TMPDIR:-/tmp}/adbpin.XXXXXX"); trap 'rm -f "$pin"' EXIT
	printf '#!/bin/sh\ncase "$1" in kill-server|start-server) exit 0;; esac\nexec "%s" -s "%s" "$@"\n' "$(command -v "$ADB")" "$SERIAL" > "$pin"; chmod +x "$pin"
	python3 "$HERE/../fullroot/fastflash-root.py" "$pin" "$IMG" "$IMG.flash-$SERIAL.state"
	echo "USERDATA written (not rebooted)" ;;
*) die "unknown command $CMD (pull-template, backup-boot, boot, root, info)" ;;
esac
