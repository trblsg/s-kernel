#!/usr/bin/env python3
"""Rewrite arm64e code in place so cores without ARMv8.3 (the S9) run it natively. This is tools/ios27/a64e/a64e-rewrite.py
with two more rewrites (PACGA, and LDRAA/LDRAB with writeback) and libcorecrypto left alone (below); docs/ios27/SPEED.md.

One instruction for one, in S_ATTR_PURE_INSTRUCTIONS sections only:
  RETAA/RETAB                 -> RET
  BRAA/BRAB/BRAAZ/BRABZ Xn    -> BR Xn
  BLRAA/BLRAB/BLRAAZ/BLRABZ Xn-> BLR Xn
  PAC*/AUT*/XPAC* (register)  -> NOP   (pointers are never signed here)
  LDRAA/LDRAB, no writeback   -> LDR (offset 0..32760) or LDUR (-256..-8)
  LDAPR{B,H,,} (LRCPC)        -> LDAR{B,H,,}   (stronger ordering; the Mongoose M3 lacks LRCPC)
  PACGA Xd, Xn, Xm, Rd != XZR -> AND Xd, Xn, #0xffffffff00000000
  LDRAA/LDRAB, pre-index writeback, offset -256..248, Rt != Rn -> LDR Xt, [Xn, #imm]!
Left for the kernel (sweet_a64e.c) to emulate on trap: FJCVTZS, FCADD, PACGA with Rd = XZR, LDRA* with
writeback and a register that is its own base or an offset out of range.

The PACGA rewrite and the kernel must agree: the kernel's trapped PACGA has to give Xn & 0xffffffff00000000 too, which it does with
boot-arg sweetspeed&4 (osfmk/arm64/sweet_a64e.c on the fork's scion/xnu-12377-starlte). Rewrite a root this way only for a kernel that is booted with that bit,
and put the original files back (keep a copy of each subcache first) before booting one that is not.

libcorecrypto is not rewritten unless --skip is given an empty list: its FIPS self-test checks a MAC of its own code, so
a rewritten one fails the test. This repository does not provide a way around that test.

  a64e-rewrite2.py cache DIR/dyld_shared_cache_arm64e [--only SUBSTR ...] [--skip SUBSTR ...] [--dry-run]
  a64e-rewrite2.py macho FILE ... [--dry-run]       (thin or fat; arm64e slices)
Files are modified in place (clone them first: cp -c). Needs Python 3 with numpy.

Cache mode leaves the words of the skip map alone when TEXTDATA_SKIP=skip.npy is set (textdata-guard.py map; see
docs/ios27/TEXT-DATA.md): data words of hand-written assembly that have the bit pattern of an instruction below."""
import argparse, glob, os, struct, sys
import numpy as np

NOP = 0xD503201F
RET = 0xD65F03C0


_TD = None


def textdata_idx(addr, n):
    """TEXTDATA_SKIP=skip.npy (sorted uint64 addresses, made by textdata-guard.py map): the indexes of the words of the
    n-word section at addr that are data and must be left alone; None when the variable is not set"""
    global _TD
    if _TD is None:
        p = os.environ.get('TEXTDATA_SKIP')
        _TD = np.load(p) if p else False
    if _TD is False:
        return None
    lo, hi = np.searchsorted(_TD, np.uint64(addr)), np.searchsorted(_TD, np.uint64(addr + 4 * n))
    return ((_TD[lo:hi] - np.uint64(addr)) // np.uint64(4)).astype(np.int64)


def rewrite_words(w, keep=None, rulec=False, left=None):
    """w: uint32 array (writable view). Returns a dict of counts.
    keep: indexes of words that are data (textdata_idx) and stay as they are. rulec: also leave a LDRAx / LDAPR candidate
    that has an undecodable word (reserved top-level encoding group, or high half zero) within 8 words before and
    after it (no oracle needed; for Mach-Os without a map). The skipped candidates are counted in c['data_left'] and
    their indexes appended to the list left."""
    c = {}
    hit = np.zeros(len(keep), bool) if keep is not None and len(keep) else None
    extra = []

    def cl(m, masked=False):
        """clear the data words from the candidate mask m"""
        if hit is not None:
            np.logical_or(hit, m[keep], out=hit)
            m[keep] = False
        if masked and rulec:
            d = np.flatnonzero(m)
            if len(d):
                inv = (((w >> 25) & 0xF) <= 3) | (w < 0x10000)
                cs = np.concatenate(([0], np.cumsum(inv)))
                n = len(w)
                cc = ((cs[d] - cs[np.maximum(d - 8, 0)]) >= 1) & ((cs[np.minimum(d + 9, n)] - cs[np.minimum(d + 1, n)]) >= 1)
                m[d[cc]] = False
                extra.append(d[cc])
        return m

    m = cl((w & 0xFFFFFBFF) == 0xD65F0BFF)                   # RETAA/RETAB
    w[m] = RET; c['ret'] = int(m.sum())
    m = cl((w & 0xFE9FF800) == 0xD61F0800)                   # BRA*/BLRA* (+ RETA* already gone)
    op = (w >> 21) & 3
    rn = (w >> 5) & 31
    br = m & (op == 0); blr = m & (op == 1)
    w[br] = (0xD61F0000 | (rn[br] << 5)).astype(np.uint32)
    w[blr] = (0xD63F0000 | (rn[blr] << 5)).astype(np.uint32)
    c['br'] = int(br.sum()); c['blr'] = int(blr.sum())
    m = cl(((w & 0xFFFF8000) == 0xDAC10000) & (((w >> 10) & 0x3f) <= 17))   # PAC/AUT/XPAC reg forms
    w[m] = NOP; c['pac'] = int(m.sum())
    m = (w & 0xFF200400) == 0xF8200400                       # LDRAA/LDRAB
    wb = (w >> 11) & 1
    imm = (((w >> 12) & 0x1ff) | (((w >> 22) & 1) << 9)).astype(np.int64)
    imm = np.where(imm & 0x200, imm - 0x400, imm) * 8
    rt = w & 31; rn = (w >> 5) & 31
    pos = cl(m & (wb == 0) & (imm >= 0), True)
    neg = cl(m & (wb == 0) & (imm < 0) & (imm >= -256), True)
    w[pos] = (0xF9400000 | ((imm[pos] // 8).astype(np.uint32) << 10) | (rn[pos] << 5) | rt[pos]).astype(np.uint32)
    w[neg] = (0xF8400000 | ((imm[neg] & 0x1ff).astype(np.uint32) << 12) | (rn[neg] << 5) | rt[neg]).astype(np.uint32)
    c['ldra'] = int(pos.sum() + neg.sum()); c['ldra_left'] = int(m.sum() - pos.sum() - neg.sum())
    m = cl((w & 0x3FFFFC00) == 0x38BFC000, True)             # LDAPR{B,H,,} Rt, [Xn]
    w[m] = ((w[m] & 0xC00003FF) | 0x08DFFC00).astype(np.uint32)
    c['ldapr'] = int(m.sum())
    # ios27-speed: PACGA Xd, Xn, Xm -> AND Xd, Xn, #0xffffffff00000000 (the kernel's trap emulation
    # gives the same value); Rd = 31 (XZR) left to trap (AND would write SP).
    m = cl(((w & 0xFFE0FC00) == 0x9AC03000) & ((w & 31) != 31))
    w[m] = (0x92607C00 | (((w[m] >> 5) & 31) << 5) | (w[m] & 31)).astype(np.uint32)
    c['pacga'] = int(m.sum())
    # LDRAA/LDRAB pre-index writeback, offset -256..248, Rt != Rn -> LDR Xt, [Xn, #imm]!
    m = (w & 0xFF200C00) == 0xF8200C00
    imm = (((w >> 12) & 0x1ff) | (((w >> 22) & 1) << 9)).astype(np.int64)
    imm = np.where(imm & 0x200, imm - 0x400, imm) * 8
    rt = w & 31; rn = (w >> 5) & 31
    ok = cl(m & (imm >= -256) & (imm <= 255) & (rt != rn))
    w[ok] = (0xF8400C00 | ((imm[ok] & 0x1ff).astype(np.uint32) << 12) | (rn[ok] << 5) | rt[ok]).astype(np.uint32)
    c['ldra_wb'] = int(ok.sum())
    c['pacga_left'] = int(((w & 0xFFE0FC00) == 0x9AC03000).sum())
    c['fjcvtzs_left'] = int(((w & 0xFFFFFC00) == 0x1E7E0000).sum())
    c['fcadd_left'] = int(((w & 0xBF20EC00) == 0x2E00E400).sum())
    if hit is not None or extra:
        idx = np.unique(np.concatenate(([keep[hit]] if hit is not None else []) + extra))
        if len(idx):
            c['data_left'] = len(idx)
            if left is not None:
                left.extend(int(i) for i in idx)
    return c


def add(t, c):
    for k, v in c.items():
        t[k] = t.get(k, 0) + v


def text_sections(lc, ncmds):
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                addr, size, foff = struct.unpack_from('<QQI', lc, so + 32)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if flags & 0x80000000 and size:
                    yield addr, size, foff
        off += csz


def do_cache(main, only, dry, skip=()):
    maps = []
    files = {}
    for p in sorted(glob.glob(main + '*')):
        if p.endswith(('.map', '.atlas')):
            continue
        with open(p, 'rb') as f:
            h = f.read(0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        mo, mc = struct.unpack_from('<II', h, 0x10)
        with open(p, 'rb') as f:
            f.seek(mo)
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                maps.append((a, s, p, o))
        files[p] = np.memmap(p, dtype=np.uint8, mode='r' if dry else 'r+')

    def loc(addr):
        for a, s, p, o in maps:
            if a <= addr < a + s:
                return p, o + addr - a
        return None, None

    def read(addr, n):
        p, o = loc(addr)
        return None if p is None else bytes(files[p][o:o + n])

    h = open(main, 'rb').read(0x200)
    io, ic = struct.unpack_from('<II', h, 0x1c0)
    if not ic:
        io, ic = struct.unpack_from('<II', h, 0x18)
    with open(main, 'rb') as f:
        f.seek(io)
        imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
        paths = []
        for im in imgs:
            f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())
    tot, nimg, dirty = {}, 0, 0
    for im, path in zip(imgs, paths):
        if only and not any(o in path for o in only):
            continue
        if any(k in path for k in skip):
            continue
        mh = read(im[0], 32)
        if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
            continue
        ncmds, sz = struct.unpack_from('<II', mh, 16)
        lc = read(im[0] + 32, sz)
        for addr, size, _ in text_sections(lc, ncmds):
            p, o = loc(addr)
            if p is None:
                continue
            mm = files[p]
            buf = np.array(mm[o:o + (size & ~3)]).view(np.uint32) if dry else mm[o:o + (size & ~3)].view(np.uint32)
            left = []
            add(tot, rewrite_words(buf, textdata_idx(addr, len(buf)), left=left))
            for i in left:
                print(f'textdata: {addr + 4 * i:#x} {int(buf[i]):08x} D {path}')
            dirty += size
        nimg += 1
        if not dry and dirty > 256 << 20:   # write back as it goes: a slow (USB) image would pile up dirty pages
            for mm in files.values():
                mm.flush()
            dirty = 0
    if not dry:
        for mm in files.values():
            mm.flush()
    return nimg, tot


def do_macho(path, dry):
    data = np.memmap(path, dtype=np.uint8, mode='r' if dry else 'r+')
    raw = bytes(data[:4096])
    magic = struct.unpack_from('>I', raw)[0]
    slices = []
    if magic in (0xcafebabe, 0xcafebabf):
        n = struct.unpack_from('>I', raw, 4)[0]
        for i in range(n):
            if magic == 0xcafebabe:
                ct, cs, off, size, _ = struct.unpack_from('>iiIII', raw, 8 + i * 20)
            else:
                ct, cs, off, size, _, _ = struct.unpack_from('>iiQQII', raw, 8 + i * 32)
            if ct == 0x0100000c and (cs & 0xff) == 2:
                slices.append(off)
    elif struct.unpack_from('<I', raw)[0] == 0xfeedfacf:
        if struct.unpack_from('<i', raw, 4)[0] == 0x0100000c and (struct.unpack_from('<i', raw, 8)[0] & 0xff) == 2:
            slices.append(0)
    tot = {}
    for base in slices:
        hdr = bytes(data[base:base + 32])
        ncmds, sz = struct.unpack_from('<II', hdr, 16)
        lc = bytes(data[base + 32:base + 32 + sz])
        for _, size, foff in text_sections(lc, ncmds):
            o = base + foff
            buf = np.array(data[o:o + (size & ~3)]).view(np.uint32) if dry else data[o:o + (size & ~3)].view(np.uint32)
            add(tot, rewrite_words(buf))
    if not dry:
        data.flush()
    return len(slices), tot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind', choices=['cache', 'macho'])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--skip', nargs='*', default=['libcorecrypto'],
                    help='leave images whose path contains one of these untranslated (default: libcorecrypto, whose FIPS '
                         'self-test MACs its own code; "--skip" with nothing after it rewrites it too)')
    a = ap.parse_args()
    if a.kind == 'cache':
        n, tot = do_cache(a.paths[0], a.only, a.dry_run, a.skip)
        print(f'{n} images', tot)
        if os.environ.get('TEXTDATA_SKIP'):
            print(f'textdata: {tot.get("data_left", 0)} words left alone')
    else:
        for p in a.paths:
            n, tot = do_macho(p, a.dry_run)
            print(f'{p}: {n} arm64e slice(s)', tot)


if __name__ == '__main__':
    main()
