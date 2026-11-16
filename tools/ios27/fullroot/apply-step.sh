#!/bin/bash
# apply-step.sh IMG STEP.sh [VAR=value ...] : (root) attach IMG read-write with owners, run one steps.d script on it, fsck, detach.
# For re-running a single step on a built image (steps are idempotent); the image is modified in place - clone it first (cp -c).
set -euo pipefail
[[ ${EUID} == 0 ]] || { echo "run as root" >&2; exit 2; }
IMG=$1; STEP=$2; shift 2
H=$(cd "$(dirname "$0")" && pwd); M=$(mktemp -d /tmp/applystep.XXXX)
DEV=$(hdiutil attach -nomount -nobrowse -imagekey diskimage-class=CRawDiskImage "$IMG" | awk 'NR==1 {print $1}')
trap 'umount "$M" 2>/dev/null; hdiutil detach "$DEV" >/dev/null 2>&1; rmdir "$M" 2>/dev/null' EXIT
mount -t hfs -o owners "$DEV" "$M"
env "$@" bash "$STEP" "$M"
sync; umount "$M"; fsck_hfs -n -f "/dev/r${DEV#/dev/}" 2>&1 | tail -2
