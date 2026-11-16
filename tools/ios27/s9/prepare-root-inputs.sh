#!/bin/bash
# SPDX-License-Identifier: MIT-0
# prepare-root-inputs.sh STOCK_CACHE_DIR [OVERLAY_DIR] : make the builder's own directory the root build reads ($IOS27_PRIVATE,
# default tools/ios27/fullroot/private, not in git) from YOUR stock shared cache and public sources. Unprivileged; about 3 minutes.
#   STOCK_CACHE_DIR  .../System/Library/Caches/com.apple.dyld of your SystemOS image, mounted read-only
#                    (hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint M inputs/systemos.dmg)
#   OVERLAY_DIR      the userland you built (docs/ios27/INSTALL-S9.md, "The userland"): a tree laid out like the root
#                    (usr/lib/sbca.dylib ...). Without it the root has no compositor and the screen stays on the boot console.
# What it writes:
#   herolte-tune-ios-root.sh   wardevsa's root tuning script (setup done, CommCenter out, containers skeleton), fetched from the
#                              author's public repository at a fixed commit and checked against a SHA-256; it is not copied into this one
#   patches.txt                tools/ios27/s9/patches-24A446.txt, after every expected word was found in your cache
#   undef.txt + the oracle     pipeline/cache/aot/make-oracle.sh over your cache (tables in pipeline/cache/aot/oracle/, not in git)
#   overlay/ + MANIFEST.sha256 a copy of OVERLAY_DIR with the manifest steps.d/30-overlay.sh checks
set -euo pipefail
SRC=${1:?usage: prepare-root-inputs.sh STOCK_CACHE_DIR [OVERLAY_DIR]}; OV=${2:-}
HERE=$(cd "$(dirname "$0")" && pwd); T27=$(cd "$HERE/.." && pwd); FR=$T27/fullroot
P=${IOS27_PRIVATE:-$FR/private}; PY=${PY:-python3}
TUNE_REPO=https://raw.githubusercontent.com/wardevsa/ios-a5xelte
TUNE_COMMIT=a43e3e512a2d8841678f4d4fc8dd28f513d8fbf7
TUNE_SHA256=6915152432af67affe8369ae7c79d1c3f1c2ef49f72ac1b56da7ac6eb2c7ab51
die() { echo "prepare-root-inputs: $*" >&2; exit 1; }
M=$SRC/dyld_shared_cache_arm64e
[ -f "$M" ] || die "no dyld_shared_cache_arm64e in $SRC"
"$PY" -c 'import numpy' 2>/dev/null || die "$PY has no numpy: $PY -m pip install numpy (PY=<python>)"
mkdir -p "$P"
# 1. the tuning script, from its author's repository
if [ "$(shasum -a 256 "$P/herolte-tune-ios-root.sh" 2>/dev/null | cut -c1-64)" != "$TUNE_SHA256" ]; then
	curl -fsSL "$TUNE_REPO/$TUNE_COMMIT/tools/herolte-tune-ios-root.sh" -o "$P/herolte-tune-ios-root.sh.part" || die "cannot fetch herolte-tune-ios-root.sh"
	[ "$(shasum -a 256 "$P/herolte-tune-ios-root.sh.part" | cut -c1-64)" = "$TUNE_SHA256" ] || die "herolte-tune-ios-root.sh: SHA-256 differs from the pinned one"
	mv "$P/herolte-tune-ios-root.sh.part" "$P/herolte-tune-ios-root.sh"
fi
echo "herolte-tune-ios-root.sh: wardevsa/ios-a5xelte@${TUNE_COMMIT:0:8}, SHA-256 checked"
# 2. the cache patches: every expected word must be in this cache
bad=0
while read -r va exp new rest; do
	case "$va" in '' | \#*) continue ;; esac
	got=$("$PY" -I - "$SRC" "$va" <<'PY'
import glob, struct, sys
d, va = sys.argv[1], int(sys.argv[2], 16)
for p in sorted(glob.glob(d + '/dyld_shared_cache_arm64e*')):
    if p.endswith(('.map', '.atlas', '.symbols')): continue
    with open(p, 'rb') as f:
        h = f.read(0x20)
        if not h.startswith(b'dyld_v1'): continue
        mo, mc = struct.unpack_from('<II', h, 0x10); f.seek(mo)
        for a, s, o, _, _ in [struct.unpack('<QQQII', f.read(32)) for _ in range(mc)]:
            if a <= va < a + s:
                f.seek(o + va - a); print('%08x' % struct.unpack('<I', f.read(4))[0]); sys.exit(0)
print('none')
PY
)
	if [ "$got" = "$exp" ]; then echo "patch $va: $exp found ($rest)"; else echo "patch $va: expected $exp, the cache has $got ($rest)" >&2; bad=1; fi
done < "$HERE/patches-24A446.txt"
[ $bad = 0 ] || die "this cache is not the build the patches were made for (iOS 27.0.1, 24A446, iPhone12,1)"
cp "$HERE/patches-24A446.txt" "$P/patches.txt"
# 3. the oracle tables
PY=$PY "$FR/pipeline/cache/aot/make-oracle.sh" "$M"
cp "$FR/pipeline/cache/aot/oracle/cache/undef.txt" "$P/undef.txt"
# 4. the overlay
rm -rf "$P/overlay"; mkdir -p "$P/overlay"
if [ -n "$OV" ]; then
	[ -d "$OV" ] || die "no overlay directory $OV"
	cp -R "$OV"/. "$P/overlay/"; rm -f "$P/overlay/MANIFEST.sha256"
else
	echo "NOTE: no overlay given: the root will boot without a compositor (no picture after the boot console)"
fi
(cd "$P/overlay" && find . -type f ! -name MANIFEST.sha256 | sort | while IFS= read -r f; do shasum -a 256 "$f"; done > ../overlay.manifest.tmp && mv ../overlay.manifest.tmp MANIFEST.sha256)
# 5. the Metal compositor's bundle, when the overlay has one, where steps.d/80-metal-ca.sh looks for it (METAL_CA_FILES)
rm -rf "$P/metal-ca-files"
B=System/Library/Extensions/StarbiteMetal.bundle
if [ -f "$P/overlay/$B/StarbiteMetal" ]; then
	mkdir -p "$P/metal-ca-files"; cp -R "$P/overlay/$B" "$P/metal-ca-files/"
	(cd "$P/metal-ca-files" && find StarbiteMetal.bundle -type f | sort | while IFS= read -r f; do shasum -a 256 "$f"; done > MANIFEST.sha256)
	echo "Metal bundle: found (the root can use the Metal compositor)"
else
	echo "NOTE: no StarbiteMetal.bundle in the overlay: build-root.sh will use the OpenGL ES compositor (ENABLE_METAL_CA=0)"
fi
echo "overlay: $(grep -c . "$P/overlay/MANIFEST.sha256" || true) files"
echo "ready: IOS27_PRIVATE=$P"
