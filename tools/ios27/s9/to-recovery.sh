#!/bin/bash
# SPDX-License-Identifier: MIT-0
# to-recovery.sh [PORT] : ask a Galaxy S9 that runs the iOS 27 kernel to restart into recovery (TWRP). The kernel's USB console
# (usb_gadget=5, built in) is a serial port on the Mac, /dev/cu.usbmodem*; the command is the word `return`. With one such port
# it is found; with several, name the one that is your phone (ls /dev/cu.usbmodem*; unplug and plug to see which appears).
# Without the console: Samsung's own keys, which S-Boot reads before any loader runs. Force a restart (hold Volume Down + Power
# for about 10 s, until the screen goes black), then at once hold Volume Up + Bixby + Power until TWRP's logo shows.
# The kernel also restarts into recovery by itself 24 hours after boot (sweetreboot=86400, built in).
set -euo pipefail
PORT=${1:-}
if [ -z "$PORT" ]; then
	ports=$(ls /dev/cu.usbmodem* 2>/dev/null || true); n=$(printf '%s\n' "$ports" | grep -c . || true)
	[ "$n" = 1 ] || { echo "to-recovery: $n console ports (${ports//$'\n'/ }): give the phone's as the argument" >&2; exit 1; }
	PORT=$ports
fi
[ -c "$PORT" ] || { echo "to-recovery: $PORT is not a serial port" >&2; exit 1; }
printf 'return\n' > "$PORT"
echo "sent 'return' to $PORT: the phone restarts into recovery within a few seconds"
