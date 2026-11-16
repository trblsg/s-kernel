#!/bin/bash
# verify-image.sh IMG WHICH(stage|ref): compare every file of list-1/2/3 in IMG against stage/ or ref/ (sha256, mode, flags)
cd "$(dirname "$0")"; IMG=$1; WHICH=$2; M=$PWD/mnt-verify; mkdir -p $M
hdiutil attach -owners off -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint $M $IMG >/dev/null 2>&1 || { echo attach failed; exit 1; }
bad=0; n=0
while IFS= read -r p; do
  [ -f $WHICH$p ] || continue
  n=$((n+1))
  a=$(shasum -a 256 < $M$p | cut -c1-64); b=$(shasum -a 256 < $WHICH$p | cut -c1-64)
  [ "$a" = "$b" ] || { echo "SHA MISMATCH $p"; bad=$((bad+1)); }
  sa=$(stat -f "%p %Sf" $M$p); sb=$(stat -f "%p %Sf" ref$p)
  [ "$sa" = "$sb" ] || { echo "META $p: $sa vs $sb"; bad=$((bad+1)); }
  xa=$(xattr -l $M$p 2>/dev/null | md5); xb=$(xattr -l ref$p 2>/dev/null | md5)
  [ "$xa" = "$xb" ] || { echo "XATTR $p"; bad=$((bad+1)); }
done < <(cat list-1.txt list-2.txt list-3.txt)
echo "checked $n files against $WHICH: bad=$bad"
hdiutil detach $M >/dev/null 2>&1 || hdiutil detach -force $M >/dev/null 2>&1
exit $bad
