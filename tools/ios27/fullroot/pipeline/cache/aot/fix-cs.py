#!/usr/bin/env python3
"""fix-cs.py NEW_MAIN REF_MAIN : for every subcache whose TEXT mapping was grown (rewrite-grow.py) extend its code signature."""
import sys, os, glob, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csgrow
PAGE = 0x4000
NEW, REF = sys.argv[1:3]
n = 0
for pn in sorted(glob.glob(NEW + '*')):
    if pn.endswith(('.map', '.atlas', '.a2s')) or os.path.isdir(pn): continue
    pr = os.path.join(os.path.dirname(REF), os.path.basename(pn))
    d = bytearray(open(pn, 'rb').read()); r = open(pr, 'rb').read(PAGE)
    if not d.startswith(b'dyld_v1'): continue
    mo, mc = struct.unpack_from('<II', d, 0x10); mor, mcr = struct.unpack_from('<II', r, 0x10)
    s = struct.unpack_from('<Q', r, mor + 8)[0]; s2 = struct.unpack_from('<Q', d, mo + 8)[0]
    G = s2 - s
    if G == 0: continue
    cso, css = struct.unpack_from('<QQ', d, 0x28)
    assert cso == s2 and cso + css == len(d), (pn, hex(cso), hex(css), hex(len(d)))
    blob = bytes(d[cso:cso + css])
    nb = csgrow.grow_blob(blob, s, G, bytes(d[s:s + G]), css)
    d[cso:] = nb
    struct.pack_into('<Q', d, 0x30, len(nb))
    open(pn + '.tmp', 'wb').write(d); os.replace(pn + '.tmp', pn); n += 1
    print(os.path.basename(pn), 'G', hex(G), 'css', hex(css), '->', hex(len(nb)))
print('fixed', n)
