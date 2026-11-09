#!/usr/bin/env python3
"""edit-launchd.py ROOT LISTS_DIR [--hogs] : launchd job edits of the full root (idempotent).
  1. hardware list (daemons-hw.list): jobs removed from launchd.plist, kept in launchd-disabled.plist (restorable)
  2. crash-loop list (daemons-crashloop.list) [+ daemons-hogs.list with --hogs]: same, by exact Label
  3. _PanicOnCrash dropped from every job (a crash loop must never panic the kernel on this port)
  4. launchd-override list -> /private/var/db/com.apple.xpc.launchd/disabled.plist
"""
import os, plistlib, sys
root, lists = sys.argv[1], sys.argv[2]
hogs = '--hogs' in sys.argv
def rd(p):
    return [l.split('#')[0].strip() for l in open(p) if l.split('#')[0].strip()]
lp = root + '/System/Library/xpc/launchd.plist'; dp = root + '/System/Library/xpc/launchd-disabled.plist'
raw = open(lp, 'rb').read(); d = plistlib.loads(raw)
try: dis = plistlib.load(open(dp, 'rb'))
except FileNotFoundError: dis = {}
jobs = d['LaunchDaemons']
hw = rd(lists + '/daemons-hw.list')
exact = set(rd(lists + '/daemons-crashloop.list')) | (set(rd(lists + '/daemons-hogs.list')) if hogs else set())
moved = []
for k in list(jobs):
    lab = jobs[k].get('Label', '') if isinstance(jobs[k], dict) else ''
    if any(h in k or h in lab for h in hw) or lab in exact:
        dis[k] = jobs.pop(k); moved.append(lab or k)
pc = 0
for k, j in jobs.items():
    if isinstance(j, dict) and '_PanicOnCrash' in j:
        del j['_PanicOnCrash']; pc += 1
with open(lp, 'r+b') as f:
    f.seek(0); f.write(plistlib.dumps(d, fmt=plistlib.FMT_BINARY)); f.truncate()
with open(dp, 'wb') as f:
    f.write(plistlib.dumps(dis, fmt=plistlib.FMT_BINARY))
os.chmod(dp, 0o644); os.chown(dp, 0, 0)
print(f'launchd.plist: {len(moved)} jobs moved to launchd-disabled.plist ({len(jobs)} left), _PanicOnCrash dropped from {pc}')
print('  moved:', ' '.join(sorted(moved)))
miss = sorted(x for x in exact if x not in {v.get('Label') for v in dis.values()})
if miss: print('  not on this root:', ' '.join(miss))
ov = root + '/private/var/db/com.apple.xpc.launchd/disabled.plist'
os.makedirs(os.path.dirname(ov), exist_ok=True)
try: o = plistlib.load(open(ov, 'rb'))
except FileNotFoundError: o = {}
for l in rd(lists + '/launchd-override-disabled.list'):
    lab, val = l.split(); o[lab] = (val == 'true')
open(ov, 'wb').write(plistlib.dumps(o, fmt=plistlib.FMT_BINARY)); os.chmod(ov, 0o644); os.chown(ov, 0, 0)
print('disabled.plist overrides:', len(o))
