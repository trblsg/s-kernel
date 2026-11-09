#!/bin/bash
# 60-wifi-firmware.sh ROOT : the BCM4361's Wi-Fi firmware, NVRAM and CLM into ROOT/private/var/hakios/wifi/, where the iOS 27
# kernel's Wi-Fi (bsd/dev/starlte_wifi.c, boot-args wifi_pcie=6 brcmfmac=N) reads them under bcmdhd's names. Idempotent.
# The files are the phone's own vendor files (Samsung's / Broadcom's), never in any repository: taken from WIFI_FW_DIR
# (default ~/s9/agents/ios27-wifi/fw, filled from the phone's VENDOR / scion-starlte tools/fetch-firmware.sh) and checked
# against scion-starlte's firmware/wifi/SHA256SUMS before anything is written. Optional extras in WIFI_FW_DIR, copied when
# there: mac.info (the phone's /efs/wifi/.mac.info) and network (two lines: name, passphrase; PRIVATE: the image then carries it).
# Needs root (run by build-full-root.sh under sudo); ROOT is the mounted root volume (owners on).
set -euo pipefail
R=$1
FW=${WIFI_FW_DIR:-/Users/dylan/s9/agents/ios27-wifi/fw}
SUMS=${WIFI_FW_SUMS:-/Users/dylan/s9/agents/ios27-wifi/scion-starlte/firmware/wifi/SHA256SUMS}
D=$R/private/var/hakios/wifi
[[ -d $R/private/var ]] || { echo "60-wifi-firmware: $R is not a root volume (no private/var)" >&2; exit 1; }
(cd "$FW" && shasum -a 256 -c "$SUMS" >/dev/null) || { echo "60-wifi-firmware: $FW does not match $SUMS" >&2; exit 1; }
mkdir -p "$D"
chown 0:0 "$R/private/var/hakios" "$D" 2>/dev/null || true
chmod 755 "$R/private/var/hakios" "$D"
n=0
for f in $(awk '{print $2}' "$SUMS") mac.info network; do
	[[ -f $FW/$f ]] || continue
	if ! cmp -s "$FW/$f" "$D/$f"; then cp "$FW/$f" "$D/$f"; n=$((n + 1)); fi
	chown 0:0 "$D/$f" 2>/dev/null || true
	case $f in network) chmod 600 "$D/$f" ;; *) chmod 644 "$D/$f" ;; esac
done
echo "60-wifi-firmware: $n file(s) written to /private/var/hakios/wifi ($(ls "$D" | wc -l | tr -d ' ') there)"
