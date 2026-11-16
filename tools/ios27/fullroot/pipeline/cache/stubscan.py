import sys, struct, glob, mmap, numpy as np
base = sys.argv[1]; fix = '--fix' in sys.argv
files = [f for f in sorted(glob.glob(base + '*')) if not f.endswith(('.map', '.atlas', '.symbols'))]
maps = []
for f in files:
    fh = open(f, 'r+b' if fix else 'rb'); mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_WRITE if fix else mmap.ACCESS_READ)
    mo, mc = struct.unpack_from('<II', mm, 0x10)
    for i in range(mc):
        a, s, o, mx, ip = struct.unpack_from('<QQQII', mm, mo + i * 32)
        maps.append((a, s, o, mm, f, ip))
def rd(addr, n):
    for a, s, o, mm, f, ip in maps:
        if a <= addr < a + s: return mm[o + addr - a:o + addr - a + n]
main = maps[0][3]
io, ic = struct.unpack_from('<II', main, 0x1c0)
if io == 0: io, ic = struct.unpack_from('<II', main, 0x18)
texts = []
for i in range(ic):
    addr = struct.unpack_from('<Q', main, io + i * 32)[0]
    h = rd(addr, 32); ncmds, sz = struct.unpack_from('<II', h, 16); lc = rd(addr + 32, sz); off = 0
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', lc, off)
        if cmd == 0x19 and lc[off+8:off+14] == b'__TEXT':
            va, vs = struct.unpack_from('<QQ', lc, off + 24); texts.append((va, va + vs))
        off += cs
texts.sort(); print(len(texts), 'images')
tot_in = tot_out = 0; regions = []
for a, s, o, mm, f, ip in maps:
    if not (ip & 4): continue
    w = np.frombuffer(mm, dtype=np.uint32, count=s // 4, offset=o)
    m = ((w & 0xFE9FF800) == 0xD61F0800) | ((w & 0xFFFFFBFF) == 0xD65F0BFF)
    idx = np.nonzero(m)[0]
    if len(idx) == 0: continue
    va = a + idx.astype(np.uint64) * 4
    starts = np.array([t[0] for t in texts], dtype=np.uint64); ends = np.array([t[1] for t in texts], dtype=np.uint64)
    k = np.searchsorted(starts, va, side='right') - 1
    inside = (k >= 0) & (va < ends[np.maximum(k, 0)])
    tot_in += int(inside.sum()); tot_out += int((~inside).sum())
    if (~inside).any():
        ov = va[~inside]; regions.append((f.split('/')[-1], hex(int(ov.min())), hex(int(ov.max())), int((~inside).sum())))
        if fix:
            wf = np.frombuffer(mm, dtype=np.uint32, count=s // 4, offset=o).copy()
            sel = idx[~inside]; ww = wf[sel]
            ret = (ww & 0xFFFFFBFF) == 0xD65F0BFF
            op = (ww >> 21) & 3; rn = (ww >> 5) & 31
            new = np.where(ret, np.uint32(0xD65F03C0), np.where(op == 0, 0xD61F0000 | (rn << 5), np.where(op == 1, 0xD63F0000 | (rn << 5), ww))).astype(np.uint32)
            for j, v in zip(sel, new): struct.pack_into('<I', mm, o + int(j) * 4, int(v))
print('BRA*/RETA* inside image __TEXT', tot_in, 'outside (stub islands)', tot_out)
for r in regions[:20]: print(r)
if fix:
    for a, s, o, mm, f, ip in maps: mm.flush()
