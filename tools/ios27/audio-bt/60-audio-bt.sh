#!/bin/bash
# 60-audio-bt.sh ROOT : the Galaxy S9's audio and Bluetooth on an iOS 27 root (Scion, private). OFF unless AUDIO_BT=1
# (unproven pieces stay out of the release candidate). Idempotent. Needs the kernel's abox=8 (+ max98512, abox_rmem=1 or
# extra_ram) for sound and bt=7 for Bluetooth; without them the plug-in finds no /dev/hakios-audio and bluetoothd no UART.
#   AUDIO_BT=1            do it
#   OS_MNT=<dir>          the IPSW's OS image mounted (System/Library/Audio and the jobs, if the root lacks them)
#   HCD=<file>            the phone's own Broadcom patch, bcm4361B2_semco.hcd (or _murata) from its VENDOR partition
#                         (both test phones: semco); installed as /private/var/hakios/bt/<its name> for bt=6
#   FILES=<dir>           what build.sh made (default build/ios27-audio-bt/files of this checkout)
set -euo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd); F=${FILES:-$(cd "$H/../../.." && pwd)/build/ios27-audio-bt/files}
[[ ${AUDIO_BT:-0} == 1 ]] || { echo "60-audio-bt: AUDIO_BT not set: nothing done"; exit 0; }
[[ -d $R/System/Library ]] || { echo "60-audio-bt: $R is not an iOS root" >&2; exit 1; }
A=$R/System/Library/Audio
if [[ ! -d $A/Plug-Ins/HAL/BTAudioHALPlugin.driver ]]; then
	[[ -n ${OS_MNT:-} && -d $OS_MNT/System/Library/Audio ]] || { echo "60-audio-bt: $A missing and no OS_MNT" >&2; exit 1; }
	/usr/bin/rsync -aH "$OS_MNT/System/Library/Audio/" "$A/"
	echo "60-audio-bt: System/Library/Audio from the IPSW ($(du -sh "$A" | cut -f1))"
fi
rm -rf "$A/Plug-Ins/HAL/HakiosAudio.driver"; cp -R "$F/HakiosAudio.driver" "$A/Plug-Ins/HAL/"
install -m 755 "$F/hakios-inject" "$R/usr/libexec/hakios-inject"
install -m 755 "$F/hakios.bluetool.dylib" "$F/hakios.bluetoothd.dylib" "$R/usr/lib/"
mkdir -p "$R/private/etc/bluetool"; install -m 644 "$F"/bluetool/*.script "$R/private/etc/bluetool/"
if [[ -n ${HCD:-} ]]; then
	mkdir -p "$R/private/var/hakios/bt"; install -m 644 "$HCD" "$R/private/var/hakios/bt/$(basename "$HCD")"
else
	echo "60-audio-bt: no HCD given: bt=6 will find no patch (put the phone's own .hcd in /private/var/hakios/bt/)"
fi
[[ ${AUDIO_BT_LOG:-0} == 1 ]] && { mkdir -p "$R/private/var/local/oslog"; chmod 777 "$R/private/var/local/oslog"; }
/usr/bin/python3 "$H/edit-launchd-audio-bt.py" "$R" ${OS_MNT:+"$OS_MNT/System/Library/xpc/launchd.plist"}
echo "60-audio-bt: done"
