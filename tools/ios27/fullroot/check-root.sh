#!/bin/bash
# check-root.sh MOUNTED_ROOT : offline sanity of a built (or owner) root. Exit 0 only if every hard check passes.
set -uo pipefail
R=$1; H=$(cd "$(dirname "$0")" && pwd); PY=${PY:-python3}
fail=0; ok() { echo "ok    $*"; }; bad() { echo "FAIL  $*"; fail=1; }; info() { echo "info  $*"; }
for f in sbin/launchd System/Library/CoreServices/SpringBoard.app/SpringBoard usr/libexec/backboardd Applications/Preferences.app/Preferences \
         usr/lib/s.dylib usr/lib/m.dylib usr/lib/b.dylib usr/local/starbite/lib/libOSMesa.8.dylib usr/local/starbite/lib/libsbgl.dylib \
         System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e.01 \
         private/var/keybags/systembag.kb System/Library/xpc/launchd.plist System/Library/xpc/launchd-disabled.plist; do
    [[ -e $R/$f ]] && ok "present $f" || bad "missing $f"; done
# full-ness: nothing trimmed, symbols out
nl=$(find "$R/System/Library" -name '*.lproj' -type d ! -name 'en.lproj' ! -name Base.lproj ! -name English.lproj ! -name 'en_*.lproj' 2>/dev/null | wc -l | tr -d ' ')
ne=$(find "$R/System" -name '*.espresso.*' 2>/dev/null | wc -l | tr -d ' '); nm=$(find "$R/System" -name '*.mlmodelc' -type d 2>/dev/null | wc -l | tr -d ' ')
info "non-English .lproj dirs under /System/Library: $nl ; espresso files: $ne ; mlmodelc dirs: $nm"
for d in LinguisticData PreinstalledAssetsV2 Health NanoTimeKit Assistant OnBoardingBundles Audio; do [[ -d $R/System/Library/$d ]] && ok "kept /System/Library/$d" || info "absent /System/Library/$d (trimmed build or not in this IPSW)"; done
ns=$(find "$R/private/preboot" "$R/System" -name '*.symbols' 2>/dev/null | wc -l | tr -d ' '); [[ $ns == 0 ]] && ok "no .symbols files" || info ".symbols files present: $ns"
info "apps in /Applications: $(ls "$R/Applications" | grep -c '\.app$') ; staged_system_apps left: $(ls "$R/private/var/staged_system_apps" 2>/dev/null | grep -c '\.app$')"
# cache: root copy hard-linked to the cryptex copy, mapping/grow state, patched words
C=$R/System/Library/Caches/com.apple.dyld; K=$R/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
nf=$(ls "$C" | wc -l | tr -d ' '); nk=$(ls "$K" | wc -l | tr -d ' '); [[ $nf == "$nk" ]] && ok "cache files: $nf (root) = $nk (cryptex)" || bad "cache file count $nf vs $nk"
nolink=0; for f in "$K"/*; do [[ $(stat -f %i "$f") == $(stat -f %i "$C/${f##*/}" 2>/dev/null) ]] || nolink=$((nolink+1)); done
[[ $nolink == 0 ]] && ok "root cache = hard links of the cryptex cache" || bad "$nolink cache files not hard-linked"
if [[ -f ${IOS27_PRIVATE:-$H/private}/patches.txt ]]; then
    while read -r va exp new rest; do [[ $va == \#* || -z $va ]] && continue
        "$PY" -I "${IOS27_PIPELINE:-$H/pipeline}/cache/cachepatch.py" "$C" $va $new $new --dry-run 2>&1 | grep -q "already patched" && ok "cache word $va = $new" || bad "cache word $va != $new"
    done < "${IOS27_PRIVATE:-$H/private}/patches.txt"
fi
# launchd sanity
/usr/bin/python3 - "$R" <<'PY' || fail=1
import os, plistlib, sys
r = sys.argv[1]; bad = 0
d = plistlib.load(open(r + '/System/Library/xpc/launchd.plist', 'rb')); dis = plistlib.load(open(r + '/System/Library/xpc/launchd-disabled.plist', 'rb'))
jobs = d['LaunchDaemons']; labels = [j.get('Label') for j in jobs.values()]
print(f'info  launchd.plist: {len(jobs)} jobs, {len(dis)} moved to launchd-disabled.plist')
dup = {l for l in labels if labels.count(l) > 1}
print('ok    no duplicate labels' if not dup else 'info  duplicate labels (stock iOS 27 launchd.plist has them): %s' % sorted(dup))
pc = [k for k, j in jobs.items() if '_PanicOnCrash' in j]
print('ok    no _PanicOnCrash' if not pc else 'FAIL  _PanicOnCrash on %d jobs' % len(pc)); bad |= bool(pc)
miss = []
for k, j in jobs.items():
    p = j.get('Program') or (j.get('ProgramArguments') or [None])[0]
    if p and not (os.path.lexists(r + p)): miss.append((j.get('Label'), p))
print('info  jobs whose executable is not on the root: %d %s' % (len(miss), [m[0] for m in miss][:12]))
both = [dis[k].get('Label') for k in dis if k in jobs]; print('ok    disabled jobs not in launchd.plist' if not both else 'FAIL  in both: %s' % both); bad |= bool(both)
for need in ('com.apple.backboardd', 'com.apple.SpringBoard', 'com.apple.configd', 'com.apple.lsd', 'com.apple.runningboardd', 'com.herolte.autorun'):
    has = any(j.get('Label') == need for j in jobs.values()); print(('ok    ' if has else 'FAIL  ') + 'job ' + need); bad |= (not has)
for gone in ('com.apple.CommCenter', 'com.apple.bluetoothd', 'com.apple.imagent', 'com.apple.rapportd', 'com.apple.terminusd'):
    has = any(j.get('Label') == gone for j in jobs.values()); print(('FAIL  still enabled: ' if has else 'ok    off: ') + gone); bad |= has
ov = plistlib.load(open(r + '/private/var/db/com.apple.xpc.launchd/disabled.plist', 'rb')); print('info  launchctl overrides:', len(ov))
sys.exit(1 if bad else 0)
PY
# prefs
/usr/bin/python3 - "$R" <<'PY' || fail=1
import plistlib, sys
P = sys.argv[1] + '/private/var/mobile/Library/Preferences/'
a = plistlib.load(open(P + 'com.apple.Accessibility.plist', 'rb'))
g = plistlib.load(open(P + '.GlobalPreferences.plist', 'rb'))
bad = a.get('ReduceMotionEnabled') is not False or a.get('ReduceTransparencyEnabled') is not False or g.get('CAEnableIOSurfaceCompression') is not False
print('FAIL  prefs' if bad else 'ok    prefs: ReduceMotion/Transparency OFF, CA compression off'); sys.exit(1 if bad else 0)
PY
# overlay + Mach-O pass idempotent
(cd "$H/../overlay" 2>/dev/null && true); O=${OVERLAY:-${IOS27_PRIVATE:-$H/private}/overlay}
n=0; while IFS= read -r l; do h=${l%% *}; f=${l#*  }; f=${f#./}; [[ $(shasum -a 256 "$R/$f" 2>/dev/null | cut -c1-64) == "$h" ]] || { n=$((n+1)); echo "      overlay differs: $f"; }; done < "$O/MANIFEST.sha256"
[[ $n == 0 ]] && ok "overlay files all present with matching sha256" || bad "$n overlay files differ"
r=$("$PY" -I "${IOS27_PIPELINE:-$H/pipeline}/root/macho-rewrite.py" "${IOS27_PIPELINE:-$H/pipeline}/cache/a64e-rewrite.py" "$R" --dry-run 2>&1 | tail -1); echo "info  Mach-O a64e dry run: $r"
case $r in *"'ret': 0, 'br': 0, 'blr': 0, 'pac': 0,"*"errors 0") ok "Mach-O a64e rewrite is complete (0 sites left, 0 errors)";; *) bad "Mach-O a64e rewrite incomplete";; esac
df -h "$R" | tail -1 | sed 's/^/info  /'; echo "info  inodes/files: $(find "$R" -xdev 2>/dev/null | wc -l | tr -d ' ') entries"
[[ $fail == 0 ]] && echo "CHECK-ROOT: ALL HARD CHECKS PASSED" || echo "CHECK-ROOT: FAILURES"; exit $fail
