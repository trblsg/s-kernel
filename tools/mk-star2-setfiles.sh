#!/bin/bash
# SPDX-License-Identifier: MIT-0
# tools/mk-star2-setfiles.sh [DIRECTORY]: the two generated headers the Galaxy S9+'s camera driver includes
# (drivers/camera/exynos/hakios.exynos_camera_star2.c): hakios.s5k2l3_setfiles.h and hakios.s5k3m3_setfiles.h.
#
# They are made from three headers of Samsung's GPL-2.0 kernel for the Exynos 9810 (the camera sensors' register tables,
# which also hold each sensor's RAM patch, written to it at start through its indirect data port). That is firmware in
# substance, so neither Samsung's headers nor the two made from them are in this repository: like brcmfmac
# (tools/mk-brcmfmac.sh) they are fetched when a kernel that needs them is built, from a public source at a pinned
# commit, checked by their SHA-256, and what is made stays in the build directory (xnu-deps/build/star2-camera, which git
# ignores). xnu-build.sh runs this for a star2lte kernel with the camera driver and copies the result into the kernel tree.
#
# The source: Anan Jaser's Apollo kernel, github.com/ananjaser1211/Apollo, at the commit below (GPL-2.0;
# drivers/media/platform/exynos/fimc-is2/sensor/module_framework/cis/). Without a network: put the three files, as they
# are at that commit, into xnu-deps/build/star2-camera/src/ and run this again; their hashes are checked all the same.
#
# The headers are made by tools/extract-2l3-setfile.py and tools/extract-3m3-setfile.py (Anan Jaser's), unchanged.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/xnu-deps/build/star2-camera}
COMMIT=30656faf075847e98f0f60a8b4282a25c7e23f80
BASE=https://raw.githubusercontent.com/ananjaser1211/Apollo/$COMMIT/drivers/media/platform/exynos/fimc-is2/sensor/module_framework/cis
FILES="fimc-is-cis-2l3-setA.h:1ac924a6e98f8ae52f00bc1f0be05b108ebb1eaee6479efc4d63084069fd8d65
fimc-is-cis-2l3-setB.h:cf42c93070f97e3c71db664eddbe288ec1789223b0318d1d233d43bf6052a8f2
fimc-is-cis-3m3-setA.h:cc3de932091e06bdab0fa089985b271ecd3faee368c20d0604e1cf8d420eca3b"

sha() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
mkdir -p "$OUT/src"
for entry in $FILES; do
	name=${entry%%:*}; want=${entry##*:}; file=$OUT/src/$name
	if [ ! -f "$file" ] || [ "$(sha "$file")" != "$want" ]; then
		echo "mk-star2-setfiles: fetching $name (Apollo $COMMIT)"
		curl -sfL -o "$file.part" "$BASE/$name" && mv "$file.part" "$file" || {
			rm -f "$file.part"
			echo "mk-star2-setfiles: cannot fetch $BASE/$name" >&2
			echo "mk-star2-setfiles: fetch that file by hand (GPL-2.0, Samsung's, as carried in Apollo at commit $COMMIT)" >&2
			echo "mk-star2-setfiles: and put it at $file (SHA-256 $want), then build again" >&2
			exit 1
		}
	fi
	got=$(sha "$file")
	[ "$got" = "$want" ] || { echo "mk-star2-setfiles: $file is not the file of commit $COMMIT (SHA-256 $got, want $want)" >&2; exit 1; }
done
python3 "$ROOT/tools/extract-2l3-setfile.py" "$OUT/src/fimc-is-cis-2l3-setA.h" "$OUT/src/fimc-is-cis-2l3-setB.h" > "$OUT/hakios.s5k2l3_setfiles.h.new"
python3 "$ROOT/tools/extract-3m3-setfile.py" "$OUT/src/fimc-is-cis-3m3-setA.h" > "$OUT/hakios.s5k3m3_setfiles.h.new"
for h in hakios.s5k2l3_setfiles.h hakios.s5k3m3_setfiles.h; do
	if cmp -s "$OUT/$h.new" "$OUT/$h" 2>/dev/null; then rm "$OUT/$h.new"; else mv "$OUT/$h.new" "$OUT/$h"; fi
done
echo "mk-star2-setfiles: $OUT/hakios.s5k2l3_setfiles.h and hakios.s5k3m3_setfiles.h are current"
