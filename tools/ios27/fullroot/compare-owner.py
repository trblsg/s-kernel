#!/usr/bin/env python3
"""compare-owner.py MINE_ROOT OWNER_ROOT CHANGED_LIST... : for every path in the inv-diff *.changed lists (files the owner build changed
vs the IPSW; sys.changed paths live under /private/preboot/Cryptexes/OS in the root) compare content (sha256) of the two roots."""
import hashlib, os, sys
mine, owner = sys.argv[1:3]
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()
same = diff = absent = 0; bad = []
for lst in sys.argv[3:]:
    pre = '/private/preboot/Cryptexes/OS' if lst.endswith('sys.changed') else ''
    for line in open(lst):
        r = line.rstrip('\n').split('\t')[0]
        if '.symbols' in r: continue
        a, b = mine + pre + r, owner + pre + r
        if not os.path.isfile(a) or os.path.islink(a) or not os.path.exists(b): absent += 1; bad.append(('absent', r)); continue
        if os.path.getsize(a) != os.path.getsize(b) or sha(a) != sha(b): diff += 1; bad.append(('differs', r))
        else: same += 1
print(f'identical {same}, differ {diff}, absent/other {absent}')
for k, r in bad[:40]: print(' ', k, r)
