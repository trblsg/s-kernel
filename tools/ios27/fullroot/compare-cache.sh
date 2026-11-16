#!/bin/bash
# compare-cache.sh DIR_A DIR_B : per-file cmp of two dyld cache directories (file set, size, byte differences)
A=$1; B=$2; nd=0; n=0
for f in "$A"/dyld_shared_cache_arm64e*; do
    b=${f##*/}; n=$((n+1))
    [[ $b == *.symbols ]] && continue
    if [[ ! -f $B/$b ]]; then echo "only in A: $b"; nd=$((nd+1)); continue; fi
    if ! cmp -s "$f" "$B/$b"; then c=$(cmp -l "$f" "$B/$b" 2>&1 | wc -l | tr -d ' '); echo "DIFF $b size $(stat -f %z "$f") vs $(stat -f %z "$B/$b"), $c differing bytes (or size mismatch)"; nd=$((nd+1)); fi
done
for f in "$B"/dyld_shared_cache_arm64e*; do [[ -f $A/${f##*/} ]] || { echo "only in B: ${f##*/}"; nd=$((nd+1)); }; done
echo "compared $n files, $nd differ"
