#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-loader.sh : xnuloader/sboot/out/xnu_boot.Image (the loader S-Boot starts from BOOT) built on the Mac itself, with zig as the
# aarch64 bare-metal compiler and linker. xnuloader/sboot/build.sh wants a GCC-style cross toolchain ($CROSS); this gives it one made
# of five small wrappers around `python3 -m ziglang` (pip install ziglang; 0.17.1 is what was used) and Xcode's llvm-nm/llvm-objdump.
# No Linux VM is needed. With a real cross GCC instead: CROSS=aarch64-elf- sh xnuloader/sboot/build.sh.
#   [PY=<python with ziglang>] tools/ios27/s9/build-loader.sh   -> prints the Image's path and SHA-256
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
PY=${PY:-python3}
"$PY" -m ziglang version >/dev/null 2>&1 || { echo "build-loader: $PY has no ziglang module: $PY -m pip install ziglang" >&2; exit 1; }
NM=$(xcrun --find llvm-nm); OBJDUMP=$(xcrun --find llvm-objdump)
T=$(mktemp -d "${TMPDIR:-/tmp}/zigcross.XXXXXX"); trap 'rm -rf "$T"' EXIT
# gcc: zig's clang; the two GCC-only options are dropped and -mcpu takes zig's spelling (cortex_a53)
cat > "$T/aarch64-zig-gcc" <<W
#!/bin/bash
a=(); for x in "\$@"; do case "\$x" in -fno-tree-*) ;; -mcpu=*) a+=("-mcpu=\$(echo "\${x#-mcpu=}" | tr - _)");; *) a+=("\$x");; esac; done
exec "$PY" -m ziglang cc -target aarch64-freestanding-none -Wno-unknown-warning-option -Wno-ignored-optimization-argument -fno-sanitize=all -g0 "\${a[@]}" -Wno-error
W
cat > "$T/aarch64-zig-ld" <<W
#!/bin/bash
a=(); for x in "\$@"; do case "\$x" in --no-warn-rwx-segments) ;; *) a+=("\$x");; esac; done
exec "$PY" -m ziglang ld.lld "\${a[@]}"
W
printf '#!/bin/bash\nexec "%s" -m ziglang objcopy "$@"\n' "$PY" > "$T/aarch64-zig-objcopy"
printf '#!/bin/bash\nexec "%s" "$@"\n' "$NM" > "$T/aarch64-zig-nm"
# readelf -r is only asked whether an absolute relocation is left: llvm-objdump -r answers the same question
printf '#!/bin/bash\nexec "%s" -r "$@" | sed "s/R_AARCH64_ABS64/ABS64/"\n' "$OBJDUMP" > "$T/aarch64-zig-readelf"
chmod +x "$T"/aarch64-zig-*
CROSS="$T/aarch64-zig-" sh "$ROOT/xnuloader/sboot/build.sh"
I=$ROOT/xnuloader/sboot/out/xnu_boot.Image
[ -f "$I" ] || { echo "build-loader: no $I" >&2; exit 1; }
shasum -a 256 "$I"
