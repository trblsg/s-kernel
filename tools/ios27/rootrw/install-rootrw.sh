#!/bin/bash
# install-rootrw.sh ROOTIMG [--only LISTFILE] [--undo]
#
# Writes the staged rewritten binaries (stage/<abs path>, described by manifest.json) into a raw HFS+ root image,
# IN PLACE (`cat new > target`: same inode, so owner/mode/xattrs/flags stay), keeping the first-time original of
# every file under orig/<abs path>; verifies sha256 after every write; syncs; detaches.
#   --only LISTFILE   only the root-relative paths listed (one per line, e.g. list-1.txt)
#   --undo            put orig/<path> back instead (every file that has an orig/ copy, or only those in LISTFILE)
# Safety: NEVER run this on the shared images. The image must live under this directory (a `cp -c` private copy,
# e.g. test.img) unless FORCE_ANY_IMAGE=1 is set in the environment. A target whose current sha256 is neither the
# manifest's sha256_before nor sha256_after (nor, for --undo, the orig copy) is left alone and reported (exit 1).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
STAGE="${STAGE:-$HERE/stage}"
ORIG="${ORIG:-$HERE/orig}"
MANIFEST="${MANIFEST:-$HERE/manifest.json}"
MNT="${MNT:-$HERE/mnt-install}"
IMG=""; ONLY=""; UNDO=0
while [ $# -gt 0 ]; do
  case "$1" in
    --only) ONLY="$2"; shift 2;;
    --undo) UNDO=1; shift;;
    -*) echo "unknown option $1" >&2; exit 2;;
    *) IMG="$1"; shift;;
  esac
done
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "usage: $0 ROOTIMG [--only LISTFILE] [--undo]" >&2; exit 2; }
IMG="$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")"
case "$IMG" in
  "$HERE"/*) ;;
  *) [ "${FORCE_ANY_IMAGE:-0}" = 1 ] || { echo "refusing: $IMG is not under $HERE (use a private cp -c copy)" >&2; exit 2; };;
esac
mkdir -p "$MNT" "$ORIG"
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
# list of (path, sha_before, sha_after) from the manifest, staged files only
LIST="$(mktemp "${TMPDIR:-/tmp}/rootrw.XXXXXX")"
python3 - "$MANIFEST" "$ONLY" > "$LIST" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
only = set(l.strip() for l in open(sys.argv[2]) if l.strip()) if sys.argv[2] else None
for p, e in sorted(m['files'].items()):
    if e.get('status') != 'staged':
        continue
    if only is not None and p not in only:
        continue
    print('%s\t%s\t%s' % (p, e['sha256_before'], e['sha256_after']))
EOF
N=$(wc -l < "$LIST" | tr -d ' ')
echo "targets: $N"
[ "$N" -gt 0 ] || { rm -f "$LIST"; echo "nothing to do"; exit 0; }
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint "$MNT" "$IMG" > /dev/null 2>&1 \
  || { echo "attach failed: $IMG" >&2; rm -f "$LIST"; exit 1; }
RC=0; DONE=0; SKIP=0
while IFS=$'\t' read -r P SB SA; do
  T="$MNT$P"; S="$STAGE$P"; O="$ORIG$P"
  if [ ! -f "$T" ]; then echo "MISSING in image: $P" >&2; RC=1; continue; fi
  CUR="$(sha "$T")"
  if [ "$UNDO" = 1 ]; then
    [ -f "$O" ] || { SKIP=$((SKIP+1)); continue; }
    OS="$(sha "$O")"
    [ "$OS" = "$SB" ] || { echo "orig copy of $P does not match manifest sha256_before" >&2; RC=1; continue; }
    [ "$CUR" = "$OS" ] && { SKIP=$((SKIP+1)); continue; }
    [ "$CUR" = "$SA" ] || { echo "UNEXPECTED content in image for $P ($CUR): not touching" >&2; RC=1; continue; }
    cat "$O" > "$T" || { echo "write failed $P" >&2; RC=1; continue; }
    [ "$(sha "$T")" = "$OS" ] || { echo "VERIFY FAILED after restore: $P" >&2; RC=1; continue; }
    DONE=$((DONE+1)); continue
  fi
  [ -f "$S" ] || { echo "no staged file for $P" >&2; RC=1; continue; }
  [ "$(sha "$S")" = "$SA" ] || { echo "staged file $P does not match manifest sha256_after" >&2; RC=1; continue; }
  if [ "$CUR" = "$SA" ]; then SKIP=$((SKIP+1)); continue; fi        # already installed
  [ "$CUR" = "$SB" ] || { echo "UNEXPECTED content in image for $P ($CUR): not touching" >&2; RC=1; continue; }
  if [ ! -f "$O" ]; then                                             # first time: keep the original
    mkdir -p "$(dirname "$O")"
    cp -p "$T" "$O" || { echo "orig copy failed $P" >&2; RC=1; continue; }
    [ "$(sha "$O")" = "$SB" ] || { echo "orig copy verify failed $P" >&2; RC=1; rm -f "$O"; continue; }
  fi
  cat "$S" > "$T" || { echo "write failed $P" >&2; RC=1; continue; }
  [ "$(sha "$T")" = "$SA" ] || { echo "VERIFY FAILED after write: $P" >&2; RC=1; continue; }
  DONE=$((DONE+1))
done < "$LIST"
sync
for i in 1 2 3; do hdiutil detach "$MNT" > /dev/null 2>&1 && break; sleep 2; done
hdiutil info | grep -q "$MNT" && { hdiutil detach -force "$MNT" > /dev/null 2>&1 || echo "WARNING: still attached at $MNT" >&2; }
rm -f "$LIST"
echo "done: $DONE written/restored, $SKIP already in place/skipped, rc=$RC"
exit $RC
