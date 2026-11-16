#!/bin/bash
# SPDX-License-Identifier: MIT-0
# make-boot-image.sh : the iOS 27 BOOT image for the Galaxy S9 (SM-G960F) from the three things you made yourself:
#   --kernel K      the kernel `OS_PROFILE=ios-27 make kernel` left (./hakios-kernel)
#   --donor-adt A   the iPhone 11 device tree of YOUR OWN IPSW (inputs/n104ap.adt, tools/ios27/s9/fetch-apple-inputs.sh)
#   --template T    an Android boot image that boots on YOUR phone: a copy of its RECOVERY partition with TWRP in it
#                   (adb pull /dev/block/by-name/RECOVERY: tools/ios27/s9/flash.sh pull-template), or the TWRP .img you installed.
#                   Its header, DTBH (Samsung's device tree for S-Boot) and nothing else are kept.
#   [--cmdline S | --cmdline-file F]   default tools/ios27/cmdline/starlte-rc.txt (the rest is built into the kernel)
#   [--loader I]    default xnuloader/sboot/out/xnu_boot.Image (tools/ios27/s9/build-loader.sh)
#   [--out O]       default out/xnu-boot-ios27.img (out/ is not in git; the image holds your IPSW's device identity: never share it)
# What it does (the steps the project's test images are made with): strip -x copy of the kernel; the S9's Apple device tree
# (mkadt_starlte.py --big --bank2 --bank2-mb 1536 --iphone11, then adt-extra-ram.py: 8 cores, the 1.5 GiB window of the second DRAM
# bank, seven extra RAM ranges of the first); mkpayload.py (kernel + ADT + command line -> SWEETXNU payload, padded to 4 KiB);
# starlte-pack-boot.py (the loader as the image's kernel, the payload appended, a one-page empty ramdisk).
# Nothing here touches a phone.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); T27=$ROOT/tools/ios27
PY=${PY:-python3}
KERNEL= DONOR= TEMPLATE= LINE= OUT=$ROOT/out/xnu-boot-ios27.img LOADER=$ROOT/xnuloader/sboot/out/xnu_boot.Image
LINEFILE=$T27/cmdline/starlte-rc.txt
while [ $# -gt 0 ]; do
	case "$1" in
	--kernel) KERNEL=$2; shift ;; --donor-adt) DONOR=$2; shift ;; --template) TEMPLATE=$2; shift ;;
	--cmdline) LINE=$2; LINEFILE=; shift ;; --cmdline-file) LINEFILE=$2; shift ;;
	--loader) LOADER=$2; shift ;; --out) OUT=$2; shift ;;
	-h | --help) sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "make-boot-image: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done
die() { echo "make-boot-image: $*" >&2; exit 1; }
[ -f "$KERNEL" ] || die "--kernel: no kernel at '$KERNEL' (OS_PROFILE=ios-27 make kernel)"
[ -f "$DONOR" ] || die "--donor-adt: no device tree at '$DONOR' (tools/ios27/s9/fetch-apple-inputs.sh)"
[ -f "$TEMPLATE" ] || die "--template: no boot image at '$TEMPLATE' (tools/ios27/s9/flash.sh pull-template)"
[ -f "$LOADER" ] || die "no loader at $LOADER (tools/ios27/s9/build-loader.sh)"
if [ -n "$LINEFILE" ]; then [ -f "$LINEFILE" ] || die "no command line file $LINEFILE"; LINE=$(cat "$LINEFILE"); fi
case "$LINE" in *$'\n'*) die "the command line has more than one line" ;; esac
n=$(printf '%s' "$LINE" | wc -c | tr -d ' ')
[ "$n" -le 607 ] || die "the command line is $n bytes; the payload holds 607 (it would be cut)"
mkdir -p "$(dirname "$OUT")"
W=$(mktemp -d "${TMPDIR:-/tmp}/bootimg.XXXXXX"); trap 'rm -rf "$W"' EXIT
cp "$KERNEL" "$W/kernel.x"; xcrun strip -x "$W/kernel.x"
"$PY" "$T27/mkadt_starlte.py" --big --bank2 --bank2-mb 1536 --iphone11 "$DONOR" "$W/base.adt" >/dev/null
"$PY" "$T27/adt-extra-ram.py" "$W/base.adt" "$W/starlte.adt"
"$PY" "$T27/herolte/mkpayload.py" "$W/kernel.x" "$W/starlte.adt" "$W/payload.bin" --cmdline "$LINE" | grep -E "^(size|fixups|entry)" || true
"$PY" - "$W/payload.bin" "$LINE" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); b = p.read_bytes()
assert b[:8] == b'SWEETXNU', 'not a SWEETXNU payload'
packed = b[0x48:0x48 + 608].split(b'\0')[0].decode()
if packed != sys.argv[2]:
    sys.exit('the packed command line differs from the one given')
p.write_bytes(b + b'\0' * (-len(b) % 4096))
print(f'command line: {len(packed)} of 607 bytes, read back from the payload')
PY
head -c 4096 /dev/zero > "$W/ramdisk.bin"
"$PY" "$T27/starlte-pack-boot.py" pack "$TEMPLATE" "$LOADER" "$OUT" --append "$W/payload.bin" --ramdisk "$W/ramdisk.bin" | tail -1
shasum -a 256 "$OUT" | tee "$OUT.sha256"
echo "command line ($n bytes): $LINE"
