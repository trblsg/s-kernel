#!/bin/bash
# cache-pipeline.sh SRC_CACHE_DIR OUT_DIR   (unprivileged; Apple-derived output stays on the mini)
# iOS 27 (24A446) shared cache -> S9 (Exynos M3, ARMv8.0 + A55) ready cache, in the owner-build order:
#   1 a64e-rewrite    arm64e -> arm64 in place (RETA*/BRA*/BLRA*/PAC*/AUT*/XPAC* -> RET/BR/BLR/NOP, LDRAx, LDAPR->LDAR), libcorecrypto skipped (FIPS MAC)
#   1b stubscan --fix  the same rewrite for the stub islands (BRAA x16,x17 / RETAA outside image __TEXT; 175 words in subcache .24 alone)
#   2 lse-rewrite     LSE atomics -> LL/SC thunks in the zero padding of each image's __TEXT (hot images)
#   4 a64e-rewrite2   PACGA -> AND and LDRAx writeback -> LDR over the whole cache (needs a kernel whose pacga_hash = a & 0xffffffff00000000)
#   5 cachepatch      Settings (SystemConfiguration IOKit-quiesce block -> ret), kbd (SafariCore app-data-container -> ret),
#                     QuartzCore 0.1 s update clamp off, PosterBoard Fix A (PFPosterPath resource-values count 2 -> 1)
#   6 rewrite-grow    full AOT rewrite of every remaining non-ARMv8.0 instruction (LSE cold images, FP16, FCMA, DotProd, RDM,
#                     BF16, I8MM, SHA3, JSCVT, LRCPC2, LOR) with stubs in grown TEXT mappings; fix-cs extends the code signature
#   7 grow-check      off-phone replay of the shared-region mapping rules + re-decode of every site/thunk/body (must exit 0)
#   0b skip map       textdata-guard.py map: the addresses of the __text words that are DATA (tables of hand-written assembly) and
#                     not code, from the stock cache; steps 1..6 leave them alone (TEXTDATA_SKIP, docs/ios27/TEXT-DATA.md)
#   8 textdata-guard  check: no changed word that is neither provably code nor reviewed; verify: every mapped word of the new cache
#                     is the stock word (without a map: the reviewed data words are put back with apply, as before)
set -euo pipefail
SRC=$1; OUT=$2
PIPE=$(cd "$(dirname "$0")" && pwd); HERE=$PIPE/cache; TOOLS=${IOS27_TOOLS:-$PIPE/../../..}   # TOOLS = scion-kernel tools/ios27
PY=${PY:-python3}
mkdir -p "$OUT"; cd "$OUT"
C=$OUT/c; PRE=$OUT/pre
echo "== 0 copy stock cache $(date -u +%T)"; rm -rf "$C" "$PRE" lse.log rewrite.log; mkdir "$C"; for f in "$SRC"/dyld_shared_cache_arm64e*; do case $f in *.symbols) ;; *) cp "$f" "$C"/;; esac; done   # the 1.2 GB .symbols stays out
M=$C/dyld_shared_cache_arm64e
TD=${IOS27_PRIVATE:-$PIPE/../private}   # undef.txt (oracle: the words no decoder accepts), textdata-reviewed.list (optional; see step 8)
unset TEXTDATA_SKIP; REV=()
[[ -f $TD/textdata-reviewed.list ]] && REV=(--reviewed "$TD/textdata-reviewed.list")
echo "== 0b skip map $(date -u +%T)"
if [[ -f $TD/undef.txt ]]; then
    $PY -I $HERE/textdata-guard.py map "$SRC/dyld_shared_cache_arm64e" --undef "$TD/undef.txt" ${REV[@]+"${REV[@]}"} --out "$OUT/skip.npy"
    export TEXTDATA_SKIP=$OUT/skip.npy       # steps 1..6 leave these words (data in __text) alone
else
    echo "no $TD/undef.txt: no skip map"
fi
echo "== 1 a64e $(date -u +%T)"; nice -n 10 $PY -I $TOOLS/a64e/a64e-rewrite.py cache $M --skip libcorecrypto
echo "== 1b stub islands $(date -u +%T)"; nice -n 10 $PY -I $HERE/stubscan.py $M --fix    # BRAA*/RETA* in the small stub subcaches (outside any image __TEXT)
echo "== 2 lse $(date -u +%T)"; nice -n 10 $PY -I $TOOLS/lse/lse-rewrite.py cache $M --log "$OUT/lse.log"
$PY -I $TOOLS/lse/lse-rewrite.py verify $M --log "$OUT/lse.log"
echo "== 4 a64e-rewrite2 (PACGA/LDRA wb) $(date -u +%T)"; nice -n 10 $PY -I $TOOLS/speed/a64e-rewrite2.py cache $M
echo "== 5 cachepatch $(date -u +%T)"
CP="$PY -I $TOOLS/speed/cachepatch.py $C"
PATCHES=${IOS27_PRIVATE:-$PIPE/../private}/patches.txt   # "VA EXPECT NEW note" per line: the builder's own findings (not in the repo)
if [[ -f $PATCHES ]]; then while read -r va exp new rest; do [[ $va == \#* || -z $va ]] && continue; $CP $va $exp $new; done < "$PATCHES"; else echo "no $PATCHES: no cache word patches"; fi
cp -c -R "$C" "$PRE"                # APFS clone = pre-grow reference for fix-cs / grow-check
echo "== 6 rewrite-grow $(date -u +%T)"; nice -n 10 $PY $HERE/aot/rewrite-grow.py cache $M --log "$OUT/rewrite.log" | tee "$OUT/rewrite.out" | tail -3
$PY $HERE/aot/fix-cs.py $M $PRE/dyld_shared_cache_arm64e | tee "$OUT/fixcs.out" | tail -2
echo "== 7 grow-check $(date -u +%T)"; $PY $HERE/aot/grow-check.py $M $PRE/dyld_shared_cache_arm64e "$OUT/rewrite.log" | tee "$OUT/grow-check.out" | tail -12
echo "== 8 text data $(date -u +%T)"
if [[ -n ${TEXTDATA_SKIP:-} ]]; then     # the passes left the mapped words alone: nothing to put back, check what the build did
    $PY -I $HERE/textdata-guard.py check "$SRC/dyld_shared_cache_arm64e" "$M" --undef "$TD/undef.txt" ${REV[@]+"${REV[@]}"} | tee "$OUT/textdata.out" | tail -5
    $PY -I $HERE/textdata-guard.py verify "$SRC/dyld_shared_cache_arm64e" "$M" --map "$TEXTDATA_SKIP" 2>&1 | tee "$OUT/textdata-verify.out" | tail -5
elif [[ -f $TD/textdata-reviewed.list && -f $TD/undef.txt ]]; then
    echo "no skip map: putting the reviewed data words back (apply)"
    $PY -I $HERE/textdata-guard.py apply "$C" "$TD/textdata-reviewed.list"
    $PY -I $HERE/textdata-guard.py check "$SRC/dyld_shared_cache_arm64e" "$M" --undef "$TD/undef.txt" --reviewed "$TD/textdata-reviewed.list" | tee "$OUT/textdata.out" | tail -5
elif [[ ${ALLOW_UNCHECKED_TEXTDATA:-0} == 1 ]]; then
    echo "no $TD/undef.txt: DATA WORDS IN __text ARE NOT CHECKED (ALLOW_UNCHECKED_TEXTDATA=1)"
else
    echo "no $TD/undef.txt: no skip map and no check; run textdata-guard.py scan, review, write the list (docs/ios27/TEXT-DATA.md); ALLOW_UNCHECKED_TEXTDATA=1 skips" >&2; exit 1
fi
echo "CACHE-PIPELINE-DONE $(date -u +%T)"
