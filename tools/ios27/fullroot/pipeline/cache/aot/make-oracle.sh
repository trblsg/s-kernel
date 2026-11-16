#!/bin/bash
# SPDX-License-Identifier: MIT-0
# make-oracle.sh STOCK_CACHE_MAIN [OUT_DIR] : the instruction "oracle" tables the AOT rewrite, grow-check and textdata-guard read,
# made from YOUR OWN stock shared cache (the main file, .../com.apple.dyld/dyld_shared_cache_arm64e) with Xcode's llvm-objdump.
# Nothing of the cache is copied anywhere but OUT_DIR, which stays on your disk (default: oracle/cache beside this script, which
# .gitignore keeps out of git).
#   a53unk.txt  every distinct __text word that llvm-objdump --mcpu=cortex-a53 (ARMv8.0 + CRC) cannot decode
#   all.txt     those words decoded with every feature on: "hexword<TAB>mnemonic<TAB>operands"
#   nonv80.txt  the lines of all.txt that decode (instructions newer than ARMv8.0)
#   undef.txt   the words no decoder accepts (data, or encodings LLVM does not know): one hex word per line
# Needs: Xcode (DEVELOPER_DIR or xcode-select), python3 with numpy (PY=<python>). About two minutes.
set -euo pipefail
SRC=${1:?usage: make-oracle.sh STOCK_CACHE_MAIN [OUT_DIR]}
HERE=$(cd "$(dirname "$0")" && pwd)
D=${2:-$HERE/oracle/cache}
PY=${PY:-python3}
LSE=${LSE_REWRITE:-$HERE/../../../../lse/lse-rewrite.py}
OD=$(xcrun --find llvm-objdump); CC=$(xcrun --find clang)
[ -f "$SRC" ] || { echo "no cache at $SRC" >&2; exit 1; }
[ -f "$LSE" ] || { echo "no lse-rewrite.py at $LSE (LSE_REWRITE=<path>)" >&2; exit 1; }
mkdir -p "$D"; T=$(mktemp -d "${TMPDIR:-/tmp}/oracle.XXXXXX"); trap 'rm -rf "$T"' EXIT
# 1. the distinct words of every image's pure-instruction sections, in four files
"$PY" - "$LSE" "$SRC" "$T" <<'PY'
import importlib.util, sys
import numpy as np
spec = importlib.util.spec_from_file_location('lse', sys.argv[1]); lse = importlib.util.module_from_spec(spec); spec.loader.exec_module(lse)
c = lse.Cache(sys.argv[2], True)
parts = []
for path, base, lc, ncmds in c.images():
    for addr, size, foff in lse.text_sections(lc, ncmds):
        v = c.view(addr, size // 4 * 4)
        if v is not None:
            parts.append(np.unique(np.frombuffer(bytes(v), dtype='<u4')))
u = np.unique(np.concatenate(parts)).astype('<u4'); n = len(u)
for i in range(4):
    u[i * n // 4:(i + 1) * n // 4].tofile('%s/c%d.bin' % (sys.argv[3], i))
print('distinct __text words: %d' % n)
PY
# 2. the ones an ARMv8.0 core's decoder does not know
for i in 0 1 2 3; do
	( cd "$T" && printf '.text\n.incbin "c%d.bin"\n' $i > c$i.s && "$CC" -arch arm64 -c c$i.s -o c$i.o &&
	  "$OD" -d --mcpu=cortex-a53 c$i.o | awk '/<unknown>/ {print $2}' > a53unk$i.txt ) &
done
wait
cat "$T"/a53unk?.txt | sort -u > "$D/a53unk.txt"
"$PY" - "$D/a53unk.txt" "$T/u2.bin" <<'PY'
import sys
import numpy as np
w = np.array([int(x, 16) for x in open(sys.argv[1]).read().split()], dtype='<u4'); w.tofile(sys.argv[2])
print('not ARMv8.0: %d' % len(w))
PY
# 3. those, decoded with every feature on
( cd "$T" && printf '.text\n.incbin "u2.bin"\n' > u2.s && "$CC" -arch arm64 -c u2.s -o u2.o &&
  "$OD" -d u2.o | awk -F'\t' '/^ *[0-9a-f]+:/ {split($1, a, " "); print a[2] "\t" $2 "\t" $3}' > "$D/all.txt" )
awk -F'\t' '$2 == "<unknown>" {print $1}' "$D/all.txt" > "$D/undef.txt"
awk -F'\t' '$2 != "<unknown>"' "$D/all.txt" > "$D/nonv80.txt"
wc -l "$D/a53unk.txt" "$D/all.txt" "$D/nonv80.txt" "$D/undef.txt"
