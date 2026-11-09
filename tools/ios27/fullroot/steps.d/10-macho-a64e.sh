#!/bin/bash
# 10-macho-a64e.sh ROOT : a64e-rewrite (PAC -> plain arm64) of every arm64e Mach-O outside the dyld caches and /private/var
# (3270 files on the owner build; hardlinks counted once). Idempotent: a second run rewrites 0 files.
set -euo pipefail
R=$1; T=${IOS27_PIPELINE:-$(cd "$(dirname "$0")/.." && pwd)/pipeline}; PY=${PY:-python3}
nice -n 10 "$PY" -I "$T/root/macho-rewrite.py" "${A64E:-$T/../../a64e/a64e-rewrite.py}" "$R"
