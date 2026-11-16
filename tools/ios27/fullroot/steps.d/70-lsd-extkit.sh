#!/bin/bash
# 70-lsd-extkit.sh ROOT : ExtensionKit.plist FeatureFlag (enforce_xpc_cache_code_signing off: the kernel has no AMFI trust cache, so the signed
# extension-point cache was rejected and none of the 126 framework-declared extension points registered) + stale LaunchServices csstores dropped
# so lsd rebuilds them. From ios27-lsd-posterkit; proven on phone 1 (bp boot, 9 Nov 21:26Z): PosterKit/WidgetKit extensions register. Default ON.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
[[ ${ENABLE_LSD_EXTKIT:-1} == 1 ]] || { echo "70-lsd-extkit: off (ENABLE_LSD_EXTKIT=0)"; exit 0; }
bash "$H/lsd-extkit/step-lsd-extkit-cache.sh" "$1"
