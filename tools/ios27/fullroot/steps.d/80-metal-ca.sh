#!/bin/bash
# 80-metal-ca.sh ROOT : Metal-CA (real Liquid Glass) as the default compositor, from ios27-fable-hard (metal-ca/README.md). Needs the kernel line
# "sbgl=0 sbmtl=1" (+ sweetspeed GPU-queue bits). ENABLE_METAL_CA=0 = GL compositor (the bp comparison boot); default ON per the owner's RC decision.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
bash "$H/metal-ca/80-metal-ca.sh" "$1"
