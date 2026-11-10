#!/bin/bash
# 85-textdata-restore.sh ROOT : put back the DATA words inside __text that the cache's instruction-rewrite passes overwrote (a table
# entry with the bit pattern of an LSE / FP16 / SHA3 / LRCPC instruction was rewritten like one; docs/ios27/TEXT-DATA.md). For roots whose
# cache was built before cache-pipeline.sh had its step 8; a no-op on a cache built with it. The list is the builder's own
# ($IOS27_PRIVATE/textdata-reviewed.list, "VADDR DAMAGED STOCK note": addresses of a vendor's cache do not go into the repository).
# Idempotent: a word that holds STOCK is left alone; a word that holds neither value stops the step (another cache build).
# The cache files are edited in place (a hard-linked second copy changes with them). ENABLE_TEXTDATA_RESTORE=0 turns it off.
set -euo pipefail
R=${1:?usage: 85-textdata-restore.sh ROOT}; H=$(cd "$(dirname "$0")" && pwd)
[[ ${ENABLE_TEXTDATA_RESTORE:-1} == 1 ]] || { echo "85-textdata-restore: off (ENABLE_TEXTDATA_RESTORE=0)"; exit 0; }
LIST=${TEXTDATA_LIST:-${IOS27_PRIVATE:-$H/../private}/textdata-reviewed.list}
C=$R/System/Library/Caches/com.apple.dyld
[[ -f $C/dyld_shared_cache_arm64e ]] || { echo "85-textdata-restore: no shared cache at $C" >&2; exit 1; }
[[ -f $LIST ]] || { echo "85-textdata-restore: no $LIST (nothing restored)"; exit 0; }
${PY:-/usr/bin/python3} "$H/../pipeline/cache/textdata-guard.py" apply "$C" "$LIST"
