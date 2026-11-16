#!/bin/bash
# console-capture.sh LOCATIONID LOG : timestamped (epoch) capture of the HakiOS USB console of the phone on port LOCATIONID; re-opens the
# device after every re-enumeration (the console moves usbmodem4 -> usbmodem01 about a minute after boot). Runs until killed.
LOC=$1; LOG=$2; H=$(cd "$(dirname "$0")" && pwd)
while true; do
    D=$(python3 -I "$H/usb-console.py" loc "$LOC" | head -1)
    if [[ -n $D && -e $D ]]; then
        echo "$D" > "${LOG%.log}.dev"
        cat "$D" 2>/dev/null | perl -MTime::HiRes=time -ne 'BEGIN{$|=1} print sprintf("%.3f ", time), $_' >> "$LOG"
    fi
    sleep 0.3
done
