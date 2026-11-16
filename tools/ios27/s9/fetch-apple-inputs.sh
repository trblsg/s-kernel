#!/bin/bash
# SPDX-License-Identifier: MIT-0
# fetch-apple-inputs.sh IPSW [OUT_DIR] : take from YOUR OWN iPhone 11 restore image (iPhone12,1, iOS 27.0.1, build 24A446, downloaded
# from Apple) the four things the S9 build reads, into OUT_DIR (default ./inputs, which must stay on your disk: these are Apple's
# files and carry the image's identity; never upload or share them):
#   os.dmg, systemos.dmg, appos.dmg   the OS image and the two cryptexes (APFS images)
#   n104ap.adt                        the iPhone 11's device tree (the donor of the S9's: tools/ios27/s9/make-boot-image.sh)
# The names inside the IPSW are read from its BuildManifest (identity n104ap, "Customer Erase Install"). The OS and SystemOS images
# are Apple Encrypted Archives: their key is the one Apple's own key service gives for the URL in the archive's header (what
# `ipsw fw aea --key` asks, as any restore tool does), and macOS's /usr/bin/aea decrypts. No key is stored or shared anywhere.
# Needs: blacktop's ipsw (https://github.com/blacktop/ipsw, MIT; 3.1.743 was used) on PATH or IPSW_TOOL=<path>, macOS's aea, unzip,
# about 25 GB free. The IPSW itself: Apple's download servers only, e.g. `ipsw download ipsw --device iPhone12,1 --build 24A446`
# (it reads Apple's catalogue and downloads from Apple), or the file Finder/Apple Configurator saved.
set -euo pipefail
IPSW=${1:?usage: fetch-apple-inputs.sh IPSW [OUT_DIR]}; OUT=${2:-$PWD/inputs}
TOOL=${IPSW_TOOL:-ipsw}; WANT_BUILD=${WANT_BUILD:-24A446}; CLASS=${DEVICE_CLASS:-n104ap}
die() { echo "fetch-apple-inputs: $*" >&2; exit 1; }
[ -f "$IPSW" ] || die "no file $IPSW"
command -v "$TOOL" >/dev/null || die "no ipsw tool ($TOOL): https://github.com/blacktop/ipsw/releases, or IPSW_TOOL=<path>"
command -v aea >/dev/null || die "no aea (macOS 14 or later has /usr/bin/aea)"
mkdir -p "$OUT"; cd "$OUT"
unzip -o -q "$IPSW" BuildManifest.plist -d . || die "$IPSW has no BuildManifest.plist: not an IPSW"
names=$(/usr/bin/python3 - BuildManifest.plist "$WANT_BUILD" "$CLASS" <<'PY'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
if m['ProductBuildVersion'] != sys.argv[2]:
    sys.exit('this IPSW is build %s (%s); the S9 tools are made for %s' % (m['ProductBuildVersion'], m.get('ProductVersion'), sys.argv[2]))
for i in m['BuildIdentities']:
    info = i['Info']
    if info.get('DeviceClass') == sys.argv[3] and info.get('RestoreBehavior') == 'Erase':
        mf = i['Manifest']
        print(' '.join(mf[k]['Info']['Path'] for k in ('OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS', 'DeviceTree')))
        break
else:
    sys.exit('no %s erase identity in this IPSW: it is not an iPhone 11 (iPhone12,1) image' % sys.argv[3])
PY
) || die "BuildManifest: $names"
read -r os sys app dt <<<"$names"
echo "build $WANT_BUILD, $CLASS: OS $os, SystemOS $sys, AppOS $app, device tree $dt"
plain() { # plain NAME_IN_IPSW OUT_NAME : unzip, and decrypt when it is an .aea
	local src=$1 dst=$2
	[ -f "$dst" ] && { echo "have $dst"; return; }
	unzip -o -q "$IPSW" "$src" -d . || die "cannot unzip $src"
	case "$src" in
	*.aea)
		local key; key=$("$TOOL" fw aea --key "$src" 2>&1 | grep -o 'base64:[A-Za-z0-9+/=]*' | head -1) || true
		[ -n "$key" ] || die "no key for $src from Apple's key service (network?)"
		aea decrypt -i "$src" -o "$dst.part" -key-value "$key" || die "aea could not decrypt $src"
		rm -f "$src"; mv "$dst.part" "$dst" ;;
	*) mv "$src" "$dst" ;;
	esac
	echo "$dst: $(stat -f %z "$dst") bytes"
}
plain "$os" os.dmg; plain "$sys" systemos.dmg; plain "$app" appos.dmg
if [ ! -f n104ap.adt ]; then
	unzip -o -q "$IPSW" "$dt" -d . || die "cannot unzip $dt"
	"$TOOL" img4 im4p extract "$dt" -o n104ap.adt >/dev/null || die "ipsw could not unpack $dt"
	rm -rf "$(echo "$dt" | cut -d/ -f1)"
fi
echo "n104ap.adt: $(stat -f %z n104ap.adt) bytes"
{ echo "made by tools/ios27/s9/fetch-apple-inputs.sh from $(basename "$IPSW"), build $WANT_BUILD"; shasum -a 256 os.dmg systemos.dmg appos.dmg n104ap.adt; } > SOURCE.txt
echo "inputs ready in $OUT (Apple's files: keep them on this disk)"
