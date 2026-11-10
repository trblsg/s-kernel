#!/bin/bash
# 86-coreimage-sw-context.sh ROOT : a default CIContext in a process without a Metal device is Core Image's software context
# instead of nil (docs/ios27/TEXT-DATA.md, "the second cause"). One word of CoreImage in the shared cache, from the builder's
# private list $IOS27_PRIVATE/coreimage-sw.list ("VADDR STOCK PATCHED note"; an address of a vendor's cache does not go into the
# repository). Without the list the step does nothing. Idempotent; a word that holds neither value stops the step.
# ENABLE_CI_SW_CONTEXT=0 turns it off; REVERT=1 puts the stock word back.
set -euo pipefail
R=${1:?usage: 86-coreimage-sw-context.sh ROOT}; H=$(cd "$(dirname "$0")" && pwd)
[[ ${ENABLE_CI_SW_CONTEXT:-1} == 1 ]] || { echo "86-coreimage-sw-context: off (ENABLE_CI_SW_CONTEXT=0)"; exit 0; }
LIST=${CI_SW_LIST:-${IOS27_PRIVATE:-$H/../private}/coreimage-sw.list}
C=$R/System/Library/Caches/com.apple.dyld
[[ -f $C/dyld_shared_cache_arm64e ]] || { echo "86-coreimage-sw-context: no shared cache at $C" >&2; exit 1; }
[[ -f $LIST ]] || { echo "86-coreimage-sw-context: no $LIST (nothing patched)"; exit 0; }
${PY:-/usr/bin/python3} "$H/../pipeline/cache/textdata-guard.py" apply "$C" "$LIST" ${REVERT:+--revert}
