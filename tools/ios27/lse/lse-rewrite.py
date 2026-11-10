#!/usr/bin/env python3
"""lse-rewrite.py - ahead-of-time LSE -> LL/SC rewrite so ARMv8.0 cores (Exynos M3) run ARMv8.1+ code
without trapping (written 8 November 2026).

Every LSE atomic in pure-instruction sections becomes `B thunk`. The thunk (16 bytes, one per site, in the
zero padding after the image's __TEXT, within +-128 MiB) is
    STR x30, [sp, #-144]! ; BL body ; LDR x30, [sp], #144 ; B site+4
and `body` is shared by every site with the same encoding (6835 distinct for the iOS 27 cache). The body reads
TPIDR_EL0 (XNU keeps the CPU number in bits 0-11): CPUs 0-3 (the Cortex-A55s, which have LSE) run the original
instruction, CPUs 4-7 (the Mongoose M3s) an exclusive-load / store-exclusive loop (--no-dual: LL/SC always;
--no-thunks: the first version's self-contained per-site stubs). A thread migrated between the test and the
instruction traps once into the kernel's emulation. Sites whose image has no room for their thunks, and
images given with --skip, keep trapping.

  CAS{,A,L,AL}{B,H,,}     CASP{,A,L,AL} (W and X pairs)
  LD{ADD,CLR,EOR,SET,SMAX,SMIN,UMAX,UMIN}{,A,L,AL}{B,H,,} (and the ST<op> aliases, Rt = ZR)
  SWP{,A,L,AL}{B,H,,}
  LDAPR{B,H,,} -> LDAR{B,H,,} in place (1:1, stronger ordering)  [--ldapr; a64e-rewrite.py already does it]

Semantics (exact or stronger):
  ordering:  A -> LDAXR, L -> STLXR, AL -> LDAXR + STLXR + DMB ISH after a successful store (an LSE
             AL atomic is fully ordered; the LL/SC pair alone would let later accesses pass the store).
             A failed CAS{A,AL} keeps its acquire (LDAXR); no release on failure (as LSE).
  CASP:      a failed compare stores the loaded pair back with STXP and retries until it succeeds, so the
             returned pair is a single-copy-atomic snapshot (LDXP alone is not atomic).
  registers: every register except the documented destination(s) is preserved, NZCV included (SMAX..UMIN
             save/restore it with MRS/MSR); scratch registers (from x9-x17, never one the instruction
             names) are saved below SP lowered by 128 + frame (the red zone stays untouched, a signal
             frame cannot land on the save slots). Rn = SP is handled (address = SP + frame).
  B/H:       compares/max/min work on the zero-/sign-extended low bits; results are zero-extended like LSE.

Modes:
  lse-rewrite.py census CACHE_MAIN            counts of instruction classes an ARMv8.0 core lacks
  lse-rewrite.py cache CACHE_MAIN [--skip SUBSTR ...] [--only SUBSTR ...] [--dry-run] [--log FILE]
  lse-rewrite.py macho FILE --pool-sym SYM [--dry-run]   (test binaries: stubs go into SYM's zero area)
  lse-rewrite.py verify CACHE_MAIN --log FILE   re-decode every logged site, thunk and body against the expected words
  lse-rewrite.py restore CACHE_MAIN --log FILE  put every original word back (the stubs stay as unused bytes)
Files are modified in place (copy them first).

TEXTDATA_SKIP=skip.npy (textdata-guard.py map; docs/ios27/TEXT-DATA.md): the words of the skip map are data, not
instructions. A candidate among them is left as it is (no thunk, no body), counted as data_left and logged as
"SITE WORD D".
"""
import argparse, glob, os, struct, sys
import numpy as np

# ---------------------------------------------------------------------------------------------------
# instruction classes
M_CAS = (0x3fa07c00, 0x08a07c00)
M_CASP = (0xbfa07c00, 0x08207c00)
M_ATOM = (0x3f200c00, 0x38200000)     # LD<op>, SWP, LDAPR (o3=1 opc=4), LD64B... (filtered below)


def lse_kind(w):
    """'cas' | 'casp' | 'ldop' | 'swp' | 'ldapr' | None"""
    if (w & M_CAS[0]) == M_CAS[1]:
        return 'cas'
    if (w & M_CASP[0]) == M_CASP[1] and ((w >> 10) & 31) == 31:
        return 'casp'
    if (w & M_ATOM[0]) == M_ATOM[1]:
        o3, opc = (w >> 15) & 1, (w >> 12) & 7
        if o3 == 0:
            return 'ldop'
        if opc == 0:
            return 'swp'
        if opc == 4 and ((w >> 16) & 31) == 31 and ((w >> 22) & 3) == 2:
            return 'ldapr'
    return None


def census_words(w, c):
    def cnt(name, m):
        c[name] = c.get(name, 0) + int(m.sum())
    cas = (w & M_CAS[0]) == M_CAS[1]
    casp = ((w & M_CASP[0]) == M_CASP[1]) & (((w >> 10) & 31) == 31)
    atom = (w & M_ATOM[0]) == M_ATOM[1]
    o3, opc = (w >> 15) & 1, (w >> 12) & 7
    ldop = atom & (o3 == 0)
    cnt('lse_cas', cas)
    cnt('lse_casp', casp)
    for i, n in enumerate(('add', 'clr', 'eor', 'set', 'smax', 'smin', 'umax', 'umin')):
        cnt('lse_ld' + n, ldop & (opc == i))
    cnt('lse_ldop_rt_zr(st<op>)', ldop & ((w & 31) == 31))
    cnt('lse_swp', atom & (o3 == 1) & (opc == 0))
    ord_ = (w >> 22) & 3          # LD<op>/SWP: A=bit23 R=bit22
    cnt('lse_ldop/swp_AL', (ldop | (atom & (o3 == 1) & (opc == 0))) & (ord_ == 3))
    cnt('lse_cas_AL', cas & (((w >> 22) & 1) == 1) & (((w >> 15) & 1) == 1))
    cnt('lse_rn_sp', (cas | casp | ldop | (atom & (o3 == 1) & (opc == 0))) & (((w >> 5) & 31) == 31))
    cnt('lrcpc_ldapr', atom & (o3 == 1) & (opc == 4) & (((w >> 16) & 31) == 31) & (((w >> 22) & 3) == 2))
    cnt('lrcpc2_ldapur/stlur', (w & 0x3F200C00) == 0x19000000)
    cnt('lor_ldlar/stllr', ((w & 0x3FFFFC00) == 0x08DF7C00) | ((w & 0x3FFFFC00) == 0x089F7C00))
    cnt('rdm_sqrdmlah/sh', ((w & 0xBF20F400) == 0x2E008400) | ((w & 0xFF20F400) == 0x7E008400) |
        ((w & 0xBF00D400) == 0x2F00D000) | ((w & 0xFF00D400) == 0x7F00D000))
    cnt('dotprod_sdot/udot', ((w & 0x9FE0FC00) == 0x0E809400) | ((w & 0x9FC0F400) == 0x0F80E000))
    cnt('fp16_scalar_arith', ((w & 0xFF200C00) == 0x1EE00800) | ((w & 0xFFC00000) == 0x1FC00000) |
        ((w & 0xFF20FC07) == 0x1EE02000) | ((w & 0xFF20FC07) == 0x1EE02008) |
        (((w & 0xFF207C00) == 0x1EE04000) & ((((w >> 15) & 0x3f) <= 3) | ((((w >> 15) & 0x3f) >> 3) == 1))))
    cnt('fp16_int_conv/fmov_h', (w & 0x7FE0FC00) == 0x1EE00000)
    cnt('fp16_simd', ((w & 0x9F60C400) == 0x0E400400) | ((w & 0x9F7E0C00) == 0x0E780800))
    cnt('dpb_dc_cvap', (w & 0xFFFFFFE0) == 0xD50B7C20)
    # neither core (A55 v8.2 lacks these too): reported for completeness
    cnt('v83_fjcvtzs', (w & 0xFFFFFC00) == 0x1E7E0000)
    cnt('v83_fcadd', (w & 0xBF20EC00) == 0x2E00E400)
    cnt('v83_fcmla', ((w & 0xBF20E400) == 0x2E00C400) | ((w & 0xBF009400) == 0x2F001000))
    cnt('v84_flagm(cfinv/rmif/setf)', ((w & 0xFFFFFFFF) == 0xD500401F) | ((w & 0xFFE07C10) == 0xBA000400) |
        ((w & 0xFFFFBC1F) == 0x3A00080D))
    cnt('v85_frint32/64', (w & 0xFF7E7C00) == 0x1E284000)
    cnt('v84_fhm_fmlal', (w & 0x9FA0EC00) == 0x0E20EC00)
    cnt('sha3/sha512', ((w & 0xFFE08000) == 0xCE000000) | ((w & 0xFFE0F000) == 0xCE608000) |
        ((w & 0xFFFFF000) == 0xCEC08000) | ((w & 0xFFE00000) == 0xCE800000))


# ---------------------------------------------------------------------------------------------------
# encoders
def ldxr(size, t, n, acq):
    return 0x085F7C00 | size << 30 | (1 << 15 if acq else 0) | n << 5 | t


def stxr(size, s, t, n, rel):
    return 0x08007C00 | size << 30 | (1 << 15 if rel else 0) | s << 16 | n << 5 | t


def ldxp(sz, t, t2, n, acq):          # sz 0 = W pair, 1 = X pair
    return 0x887F0000 | sz << 30 | (1 << 15 if acq else 0) | t2 << 10 | n << 5 | t


def stxp(sz, s, t, t2, n, rel):
    return 0x88200000 | sz << 30 | (1 << 15 if rel else 0) | s << 16 | t2 << 10 | n << 5 | t


def alu(op, sf, d, n, m):            # shifted-register form, shift 0; m = 31 is ZR
    base = {'add': 0x0B000000, 'sub': 0x4B000000, 'bic': 0x0A200000, 'eor': 0x4A000000,
            'orr': 0x2A000000}[op]
    return base | sf << 31 | m << 16 | n << 5 | d


def sub_ext(d, n, m, opt):           # SUB Wd, Wn, Wm, <ext>  (opt 0 UXTB, 1 UXTH)
    return 0x4B200000 | m << 16 | opt << 13 | n << 5 | d


def cmp_ext(n, m, opt):              # CMP Wn, Wm, <ext> = SUBS WZR, Wn, Wm, ext  (opt 0 UXTB 1 UXTH 4 SXTB 5 SXTH)
    return 0x6B200000 | m << 16 | opt << 13 | n << 5 | 31


def cmp_reg(sf, n, m):               # CMP Rn, Rm
    return 0x6B000000 | sf << 31 | m << 16 | n << 5 | 31


def sxt(d, n, half):                 # SXTB/SXTH Wd, Wn = SBFM Wd, Wn, #0, #7/#15
    return 0x13000000 | (15 if half else 7) << 10 | n << 5 | d


def csel(sf, d, n, m, cond):
    return 0x1A800000 | sf << 31 | m << 16 | cond << 12 | n << 5 | d


def cbnz(sf, t, off):
    return (0xB5000000 if sf else 0x35000000) | ((off // 4) & 0x7FFFF) << 5 | t


def mov(sf, d, m):
    return (0xAA0003E0 if sf else 0x2A0003E0) | m << 16 | d


def mrs_nzcv(t):
    return 0xD53B4200 | t


def msr_nzcv(t):
    return 0xD51B4200 | t


def add_sp(d, imm):                  # ADD Xd, SP, #imm
    return 0x91000000 | imm << 10 | 31 << 5 | d


def b(off):
    assert -(1 << 27) <= off < (1 << 27), off
    return 0x14000000 | ((off // 4) & 0x3FFFFFF)


DMB_ISH = 0xD5033BBF
GT, LT, HI, LO = 0xC, 0xB, 0x8, 0x3
SCRATCH = list(range(9, 18))         # x9..x17 (x18 platform register: never)


def stub_words(w, redzone=128, sp_extra=0, ret=False, dual=False):
    """LL/SC replacement for LSE word w, without the final branch back; None if not handled.
    redzone=128, sp_extra=0: self-contained per-site stub (entered by B from the site).
    redzone=0, sp_extra=144, ret=True: shared body, entered by BL from a per-site thunk that already
    lowered SP by 144 (x30 saved there), so the caller's red zone is behind us.
    dual: the body first reads TPIDR_EL0 (XNU keeps the CPU number in bits 0-11): CPUs 0-3 (the A55s, which
    have LSE) run the original instruction itself, CPUs 4-7 (the M3s) the LL/SC loop. A thread moved to an
    M3 between the test and the instruction traps once into the kernel's LSE emulation: still exact."""
    kind = lse_kind(w)
    if kind in (None, 'ldapr'):
        return None
    rt, rn, rs = w & 31, (w >> 5) & 31, (w >> 16) & 31
    if kind == 'casp':
        if rs & 1 or rt & 1 or rs == 30 or rt == 30:
            return None                       # unallocated (odd register pair)
        names = {rs, rs + 1, rt, rt + 1, rn}
    else:
        names = {rs, rt, rn}
    free = [r for r in SCRATCH if r not in names]
    need = {'cas': 2, 'casp': 3, 'swp': 2, 'ldop': 4 if ((w >> 12) & 7) >= 4 else 3}[kind]
    if rn == 31:
        need += 1
    if len(free) < need:
        return None
    if ret and 30 in names:
        return None                           # x30 is the thunk's return address: per-site stub instead
    sv = free[:need]
    nslot = (need + 1) & ~1
    frame = redzone + 8 * nslot               # (red zone) + save slots, 16-aligned
    pro = [0xD1000000 | frame << 10 | 31 << 5 | 31]                           # SUB SP, SP, #frame
    epi = []
    for i in range(0, need, 2):
        if i + 1 < need:
            pro.append(0xA9000000 | ((i // 1) & 0x7F) << 15 | sv[i + 1] << 10 | 31 << 5 | sv[i])   # STP [SP,#8i]
            epi.append(0xA9400000 | ((i // 1) & 0x7F) << 15 | sv[i + 1] << 10 | 31 << 5 | sv[i])   # LDP
        else:
            pro.append(0xF9000000 | i << 10 | 31 << 5 | sv[i])                # STR [SP,#8i]
            epi.append(0xF9400000 | i << 10 | 31 << 5 | sv[i])                # LDR
    epi.append(0x91000000 | frame << 10 | 31 << 5 | 31)                       # ADD SP, SP, #frame
    base = rn
    if rn == 31:
        base = sv.pop()                                                       # last scratch = address
        pro.append(add_sp(base, frame + sp_extra))
    body = []
    if kind == 'cas':
        size = w >> 30
        sf = 1 if size == 3 else 0
        acq, rel = bool(w & (1 << 22)), bool(w & (1 << 15))
        t, s = sv[0], sv[1]
        cmpw = sub_ext(s, t, rs, size) if size < 2 else alu('sub', sf, s, t, rs)
        tail = [DMB_ISH] if (acq and rel) else []
        body = [ldxr(size, t, base, acq), cmpw, cbnz(sf, s, 4 * (3 + len(tail))),
                stxr(size, s, rt, base, rel), cbnz(0, s, -16)] + tail
        if rs != 31:
            body.append(mov(sf, rs, t))
    elif kind == 'casp':
        sz = (w >> 30) & 1
        acq, rel = bool(w & (1 << 22)), bool(w & (1 << 15))
        t0, t1, s = sv[0], sv[1], sv[2]
        tail = [DMB_ISH] if (acq and rel) else []
        # 0 ldxp; 1 eor; 2 cbnz fail; 3 eor; 4 cbnz fail; 5 stxp; 6 cbnz loop; [dmb]; b done;
        # fail: stxp(same values, no release); cbnz loop; done: mov; mov
        k = len(tail)
        fail = 8 + k
        body = [ldxp(sz, t0, t1, base, acq),
                alu('eor', sz, s, t0, rs), cbnz(sz, s, 4 * (fail - 2)),
                alu('eor', sz, s, t1, rs + 1), cbnz(sz, s, 4 * (fail - 4)),
                stxp(sz, s, rt, rt + 1, base, rel), cbnz(0, s, -24)] + tail + [
                b(4 * 3),                                                      # -> done
                stxp(sz, s, t0, t1, base, False), cbnz(0, s, -4 * (fail + 1)),
                mov(sz, rs, t0), mov(sz, rs + 1, t1)]
    else:
        size = w >> 30
        sf = 1 if size == 3 else 0
        acq, rel = bool(w & (1 << 23)), bool(w & (1 << 22))
        tail = [DMB_ISH] if (acq and rel) else []
        if kind == 'swp':
            t, s = sv[0], sv[1]
            body = [ldxr(size, t, base, acq), stxr(size, s, rs, base, rel), cbnz(0, s, -8)] + tail
        else:
            opc = (w >> 12) & 7
            t, u, s = sv[0], sv[1], sv[2]
            if opc < 4:
                op = ('add', 'bic', 'eor', 'orr')[opc]
                loop = [ldxr(size, t, base, acq), alu(op, sf, u, t, rs)]
                pre, post = [], []
            else:
                f = sv[3]
                signed = opc in (4, 5)
                cond = {4: GT, 5: LT, 6: HI, 7: LO}[opc]
                loop = [ldxr(size, t, base, acq)]
                if size < 2:
                    if signed:
                        loop += [sxt(u, t, size == 1), cmp_ext(u, rs, 4 + size)]
                    else:
                        loop += [cmp_ext(t, rs, size)]
                else:
                    loop += [cmp_reg(sf, t, rs)]
                loop += [csel(sf, u, t, rs, cond)]
                pre, post = [mrs_nzcv(f)], [msr_nzcv(f)]
            n = len(loop)
            body = pre + loop + [stxr(size, s, u, base, rel), cbnz(0, s, -4 * (n + 1))] + tail + post
        if rt != 31:
            body.append(mov(sf, rt, t))
    if dual:
        nat = (word_with_rn(w, base))
        pro += [0xD53BD040 | sv[0],                                            # MRS sv0, TPIDR_EL0
                0x37000000 | 2 << 19 | (3 & 0x3FFF) << 5 | sv[0],              # TBNZ sv0, #2, llsc
                nat,                                                           # the LSE op itself (A55)
                b(4 * (1 + len(body)))]                                        # -> epilogue
    return pro + body + epi + ([0xD65F03C0] if ret else [])


def word_with_rn(w, rn):
    return (w & ~(31 << 5)) | rn << 5


# ---------------------------------------------------------------------------------------------------
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


def text_seg(lc, ncmds):
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19 and lc[off + 8:off + 15] == b'__TEXT\0':
            vmaddr, vmsize = struct.unpack_from('<QQ', lc, off + 24)
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            end = vmaddr
            for s in range(nsect):
                so = off + 72 + s * 80
                a, sz = struct.unpack_from('<QQ', lc, so + 32)
                end = max(end, a + sz)
            return vmaddr, end, vmaddr + vmsize
        off += csz
    return None


class Cache:
    def __init__(self, main, dry):
        self.maps, self.files = [], {}
        for p in sorted(glob.glob(main + '*')):
            if p.endswith(('.map', '.atlas', '.a2s')):
                continue
            with open(p, 'rb') as f:
                h = f.read(0x200)
            if not h.startswith(b'dyld_v1'):
                continue
            mo, mc = struct.unpack_from('<II', h, 0x10)
            with open(p, 'rb') as f:
                f.seek(mo)
                for _ in range(mc):
                    a, s, o, maxp, initp = struct.unpack('<QQQII', f.read(32))
                    self.maps.append((a, s, p, o, initp))
            self.files[p] = np.memmap(p, dtype=np.uint8, mode='r' if dry else 'r+')
        h = open(main, 'rb').read(0x200)
        io, ic = struct.unpack_from('<II', h, 0x1c0)
        if not ic:
            io, ic = struct.unpack_from('<II', h, 0x18)
        with open(main, 'rb') as f:
            f.seek(io)
            self.imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
            self.paths = []
            for im in self.imgs:
                f.seek(im[3])
                self.paths.append(f.read(512).split(b'\0')[0].decode())

    def loc(self, addr):
        for a, s, p, o, prot in self.maps:
            if a <= addr < a + s:
                return p, o + addr - a, prot, a + s
        return None, None, None, None

    def view(self, addr, n):
        p, o, _, _ = self.loc(addr)
        return None if p is None else self.files[p][o:o + n]

    def images(self, only=(), skip=()):
        for im, path in zip(self.imgs, self.paths):
            if only and not any(x in path for x in only):
                continue
            if any(x in path for x in skip):
                continue
            mh = self.view(im[0], 32)
            if mh is None or struct.unpack_from('<I', bytes(mh))[0] != 0xfeedfacf:
                continue
            ncmds, sz = struct.unpack_from('<II', bytes(mh), 16)
            lc = bytes(self.view(im[0] + 32, sz))
            yield path, im[0], lc, ncmds


class Pool:
    """Free code space: the zero padding after each image's __TEXT sections (up to the next image's
    __TEXT or the 16 KiB-rounded segment end), inside an executable mapping of the same file."""

    def __init__(self, regions):
        self.regions = sorted(regions)        # [start, end) lists
        self.cur = [r[0] for r in self.regions]
        self.starts = [r[0] for r in self.regions]
        self.alive = list(range(len(self.regions)))
        self.alive_starts = list(self.starts)

    def free_last(self, a, nbytes):
        for j, c in enumerate(self.cur):
            if c == a + nbytes:
                self.cur[j] = a
                return

    def alloc(self, near, nbytes, align=4):
        """Space within +-(128 MiB - 64 KiB) of near (B/BL range from near and back to near+4):
        the closest region (outward from near) with room. Full regions leave the search list."""
        import bisect
        lim = (1 << 27) - 0x10000
        alive = self.alive
        i = bisect.bisect_right(self.alive_starts, near)
        lo, hi = i - 1, i
        while lo >= 0 or hi < len(alive):
            for side in (0, 1):
                k = lo if side == 0 else hi
                if k < 0 or k >= len(alive):
                    continue
                j = alive[k]
                s, e = self.regions[j]
                if (side == 0 and near - e > lim) or (side == 1 and s - near > lim):
                    if side == 0:
                        lo = -1
                    else:
                        hi = len(alive)
                    continue
                a = (self.cur[j] + align - 1) & ~(align - 1)
                if a + nbytes <= e and abs(a - near) < lim and abs(a + nbytes - near) < lim:
                    self.cur[j] = a + nbytes
                    if e - self.cur[j] < 16:
                        del alive[k]
                        del self.alive_starts[k]
                    return a
            lo -= 1
            hi += 1
        return None

    def alloc_far(self, near, nbytes, align=16):
        """Like alloc, but the farthest region in range first (shared bodies: keeps the space close to
        the sites for their thunks)."""
        import bisect
        lim = (1 << 27) - 0x10000
        alive = self.alive
        lo = bisect.bisect_left(self.alive_starts, near - lim)
        hi = bisect.bisect_right(self.alive_starts, near + lim) - 1
        while lo <= hi:
            for k in ((lo, hi) if near - self.alive_starts[lo] >= self.alive_starts[hi] - near else (hi, lo)):
                j = alive[k]
                s, e = self.regions[j]
                a = (self.cur[j] + align - 1) & ~(align - 1)
                if a + nbytes <= e and abs(a - near) < lim and abs(a + nbytes - near) < lim:
                    self.cur[j] = a + nbytes
                    if e - self.cur[j] < 16:
                        del alive[k]
                        del self.alive_starts[k]
                    return a
            # drop whichever end was tried and failed
            if near - self.alive_starts[lo] >= self.alive_starts[hi] - near:
                lo += 1
            else:
                hi -= 1
        return None

    def left(self):
        return sum(e - c for (s, e), c in zip(self.regions, self.cur))


def cache_pool(c, skip=()):
    """Skipped images (libcorecrypto: FIPS self-MAC) keep their padding untouched too."""
    texts = []
    for path, base, lc, ncmds in c.images():
        t = text_seg(lc, ncmds)
        if t:
            texts.append(t + (any(x in path for x in skip),))
    texts.sort()
    regions = []
    for i, (start, end, segend, skipped) in enumerate(texts):
        if skipped:
            continue
        nxt = texts[i + 1][0] if i + 1 < len(texts) else segend
        lim = min(nxt, (segend + 0x3fff) & ~0x3fff)
        s = (end + 63) & ~63
        p, o, prot, mend = c.loc(s)
        if p is None or not (prot & 4):
            continue
        lim = min(lim, mend)
        if lim - s < 64:
            continue
        v = np.asarray(c.view(s, lim - s))
        nz = np.flatnonzero(v)
        if len(nz):                        # something already lives there (an earlier run): skip past it
            s = (s + int(nz[-1]) + 64) & ~63
        if lim - s >= 64:
            regions.append([s, lim])
    return Pool(regions)


THUNK_SP = 144
DUAL = True        # --no-dual: LL/SC on every core
HOT = ['/usr/lib/system/', '/usr/lib/libobjc', '/usr/lib/libc++', 'CoreFoundation.framework', 'Foundation.framework',
       'QuartzCore.framework', 'UIKitCore.framework', 'CoreGraphics.framework', '/usr/lib/dyld', '/usr/lib/libSystem',
       '/usr/lib/libxpc', '/usr/lib/swift/libswiftCore', '/usr/lib/swift/', '/usr/lib/libswift', 'CoreText.framework',
       'IOSurface', 'IOKit.framework', 'BackBoardServices', 'BaseBoard', 'FrontBoard', 'RunningBoard',
       'SpringBoard', 'CoreUI.framework', 'UIFoundation', 'ImageIO.framework', 'CoreImage.framework',
       'Metal.framework', 'OpenGLES', 'libGL', 'CoreServices', 'Security.framework', 'CFNetwork',
       'libsqlite3', 'CoreData.framework', 'SwiftUI', 'Combine', 'AttributeGraph', 'GraphicsServices',
       'CoreAnimation', 'HID', 'CoreMotion', 'AXRuntime', 'Accessibility', 'TextInput', 'Preferences',
       'Settings', 'MobileCoreServices', 'libdispatch', 'libz', 'AppleJPEG', 'VideoToolbox',
       'CoreMedia.framework', 'AudioToolbox', 'CoreAudio', 'Network.framework', 'libnetwork',
       'Contacts', 'UserNotifications', 'SpringBoardServices', 'SpringBoardFoundation', 'SpringBoardHome',
       'Widgets', 'Chrono', 'PosterBoard', 'Intents', 'BoardServices', 'ProtocolBuffer', 'CoreDuet',
       'libFontParser', 'FontServices', 'CoreSVG', 'CoreVideo', 'ColorSync']


def thunk_words(thunk, body, site):
    return [0xF8000C00 | ((-THUNK_SP) & 0x1ff) << 12 | 31 << 5 | 30,          # STR x30, [SP, #-144]!
            0x94000000 | (((body - (thunk + 4)) // 4) & 0x3FFFFFF),            # BL body
            0xF8400400 | (THUNK_SP & 0x1ff) << 12 | 31 << 5 | 30,               # LDR x30, [SP], #144
            b(site + 4 - (thunk + 12))]                                        # B site + 4


class Bodies:
    def __init__(self, pool):
        self.pool, self.placed = pool, {}

    def get(self, w, near):
        for a in self.placed.get(w, ()):
            if abs(a - near) < (1 << 27) - 0x100:
                return a, 0
        words = stub_words(w, redzone=0, sp_extra=THUNK_SP, ret=True, dual=DUAL)
        if words is None:
            return None, 0
        n = 4 * len(words)
        a = self.pool.alloc_far(near, n, align=16)
        if a is None:
            return None, 0
        self.placed.setdefault(w, []).append(a)
        return a, words


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


def rewrite_section(words, addr, pool, write_at, stats, log, dry, bodies=None, far=False, keep=None):
    """words: uint32 view of a section at addr. Rewrites LSE sites. keep: indexes of words that are data (textdata_idx):
    candidates among them stay as they are, are counted in stats['data_left'] and logged as "SITE WORD D"."""
    w = words
    m = ((w & M_CAS[0]) == M_CAS[1]) | (((w & M_CASP[0]) == M_CASP[1]) & (((w >> 10) & 31) == 31)) | \
        (((w & M_ATOM[0]) == M_ATOM[1]) & ((((w >> 15) & 1) == 0) | (((w >> 12) & 7) == 0)))
    if keep is not None and len(keep):
        for i in keep[m[keep]]:
            word = int(w[i])
            if stub_words(word) is None and stub_words(word, redzone=0, sp_extra=THUNK_SP, ret=True, dual=DUAL) is None:
                continue                  # no encoding the pass can rewrite (counted as left_* below): stays as it is either way
            m[i] = False
            stats['data_left'] = stats.get('data_left', 0) + 1
            if log:
                log.write(f'{addr + 4 * int(i):#x} {word:08x} D\n')
    for i in np.flatnonzero(m):
        word = int(w[i])
        site = addr + 4 * int(i)
        kind = lse_kind(word)
        done = False
        if bodies is not None:
            th = pool.alloc_far(site, 16, align=4) if far else pool.alloc(site, 16, align=4)
            if th is not None:
                body, bw = bodies.get(word, th)
                if body is not None:
                    tw = thunk_words(th, body, site)
                    if not dry:
                        if bw:
                            write_at(body, struct.pack(f'<{len(bw)}I', *bw))
                        write_at(th, struct.pack('<4I', *tw))
                        w[i] = b(th - site)
                    if bw:
                        stats['body_bytes'] = stats.get('body_bytes', 0) + 4 * len(bw)
                    stats['thunk_bytes'] = stats.get('thunk_bytes', 0) + 16
                    done = True
                    if log:
                        log.write(f'{site:#x} {word:08x} T {th:#x} {body:#x}\n')
                else:
                    pool.free_last(th, 16)
        if not done:
            st = stub_words(word)
            if st is None:
                stats['left_' + str(kind)] = stats.get('left_' + str(kind), 0) + 1
                continue
            nbytes = 4 * (len(st) + 1)
            at = pool.alloc(site, nbytes, align=4)
            if at is None:
                stats['no_room'] = stats.get('no_room', 0) + 1
                continue
            st.append(b(site + 4 - (at + 4 * len(st))))
            if not dry:
                write_at(at, struct.pack(f'<{len(st)}I', *st))
                w[i] = b(at - site)
            stats['stub_bytes'] = stats.get('stub_bytes', 0) + nbytes
            if log:
                log.write(f'{site:#x} {word:08x} S {at:#x} {len(st)}\n')
        stats[kind] = stats.get(kind, 0) + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['census', 'cache', 'macho', 'verify', 'restore'])
    ap.add_argument('path')
    ap.add_argument('--only', nargs='*', default=[])
    ap.add_argument('--skip', nargs='*', default=['libcorecrypto'])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--log')
    ap.add_argument('--pool-sym')
    ap.add_argument('--per-image', action='store_true')
    ap.add_argument('--no-thunks', action='store_true', help='self-contained per-site stubs only (v1)')
    ap.add_argument('--no-dual', action='store_true', help='no A55 native path in the shared bodies')
    a = ap.parse_args()
    log = open(a.log, 'w') if a.log and a.mode in ('cache', 'macho') else None
    global DUAL
    DUAL = not a.no_dual

    if a.mode == 'census':
        c = Cache(a.path, True)
        tot, nimg, ninsn, per = {}, 0, 0, []
        for path, base, lc, ncmds in c.images(a.only, ()):
            ci = {}
            for addr, size, _ in text_sections(lc, ncmds):
                v = c.view(addr, size & ~3)
                if v is None:
                    continue
                w = np.asarray(v).view(np.uint32)
                ninsn += len(w)
                census_words(w, ci)
            for k, v in ci.items():
                tot[k] = tot.get(k, 0) + v
            lse = sum(v for k, v in ci.items() if k in ('lse_cas', 'lse_casp', 'lse_swp') or
                      (k.startswith('lse_ld') and '(' not in k))
            per.append((lse, path))
            nimg += 1
        print(f'{nimg} images, {ninsn} instructions')
        for k in sorted(tot):
            print(f'  {k:32s} {tot[k]:10d}  ({1000.0 * tot[k] / max(ninsn, 1):.3f}/1000)')
        if a.per_image:
            for n, p in sorted(per, reverse=True)[:40]:
                print(f'  {n:8d} {p}')
        return

    if a.mode in ('verify', 'restore'):
        # --log = a previous (non-dry) run's log. verify: every logged site is B -> thunk/stub with the
        # expected words. restore: put the original LSE words back (stubs stay as unused bytes).
        c = Cache(a.path, a.mode == 'verify' or a.dry_run)
        bad = ok = 0
        rd = lambda ad, n: struct.unpack(f'<{n}I', bytes(c.view(ad, 4 * n)))
        for line in open(a.log):
            if line.startswith('#'):
                continue
            f = line.split()
            if len(f) == 3 and f[2] == 'D':             # data word left alone: nothing to verify or restore
                continue
            site, word, typ, at = int(f[0], 16), int(f[1], 16), f[2], int(f[3], 16)
            cur = rd(site, 1)[0]
            if a.mode == 'restore':
                if cur == b(at - site):
                    if not a.dry_run:
                        c.view(site, 4)[:] = np.frombuffer(struct.pack('<I', word), dtype=np.uint8)
                    ok += 1
                else:
                    bad += 1
                continue
            good = cur == b(at - site)
            if typ == 'T':
                body = int(f[4], 16)
                good = good and list(rd(at, 4)) == thunk_words(at, body, site)
                bw = stub_words(word, redzone=0, sp_extra=THUNK_SP, ret=True, dual=DUAL)
                good = good and bw is not None and list(rd(body, len(bw))) == bw
            else:
                sw = stub_words(word)
                good = good and sw is not None and list(rd(at, len(sw))) == sw and \
                    rd(at + 4 * len(sw), 1)[0] == b(site + 4 - (at + 4 * len(sw)))
            ok += good
            bad += not good
            if not good and bad < 10:
                print('MISMATCH', line.strip())
        if a.mode == 'restore' and not a.dry_run:
            for mm in c.files.values():
                mm.flush()
        print(f'{a.mode}: {ok} ok, {bad} bad')
        return

    if a.mode == 'cache':
        c = Cache(a.path, a.dry_run)
        pool = cache_pool(c, a.skip)
        print(f'pool: {len(pool.regions)} regions, {pool.left()} bytes')

        def write_at(addr, data):
            v = c.view(addr, len(data))
            v[:] = np.frombuffer(data, dtype=np.uint8)
        bodies = None if a.no_thunks else Bodies(pool)
        stats, nimg = {}, 0
        # hot images first (what SpringBoard/backboardd/daemons run all the time), then the rest by
        # ascending LSE count, so the pool runs out on the big cold frameworks (WebCore, 3D/vision, ...)
        imgs = list(c.images(a.only, a.skip))
        def lse_count(lc, ncmds):
            n = 0
            for addr, size, _ in text_sections(lc, ncmds):
                v = c.view(addr, size & ~3)
                if v is None:
                    continue
                w = np.asarray(v).view(np.uint32)
                n += int((((w & M_CAS[0]) == M_CAS[1]) | ((w & M_ATOM[0]) == M_ATOM[1])).sum())
            return n
        def prio(path):
            for k, h in enumerate(HOT):
                if h in path:
                    return k
            return len(HOT)
        order = sorted(imgs, key=lambda x: (prio(x[0]), lse_count(x[2], x[3]) if prio(x[0]) == len(HOT) else 0))
        for path, base, lc, ncmds in order:
            before = dict(stats)
            for addr, size, _ in text_sections(lc, ncmds):
                v = c.view(addr, size & ~3)
                if v is None:
                    continue
                w = v.view(np.uint32) if not a.dry_run else np.array(v).view(np.uint32)
                rewrite_section(w, addr, pool, write_at, stats, log, a.dry_run, bodies, keep=textdata_idx(addr, len(w)), far=prio(path) < len(HOT))
            nimg += 1
            if log:
                miss = stats.get('no_room', 0) - before.get('no_room', 0)
                log.write(f'# {path} no_room {miss}\n')
        if not a.dry_run:
            for mm in c.files.values():
                mm.flush()
        print(f'{nimg} images', stats, f'pool left {pool.left()}')
        if os.environ.get('TEXTDATA_SKIP'):
            print(f'textdata: {stats.get("data_left", 0)} words left alone')
        return

    # macho (thin arm64/arm64e, test binaries): stubs into the zero-filled pool symbol
    data = np.memmap(a.path, dtype=np.uint8, mode='r' if a.dry_run else 'r+')
    raw = bytes(data[:65536])
    assert struct.unpack_from('<I', raw)[0] == 0xfeedfacf
    ncmds, sz = struct.unpack_from('<II', raw, 16)
    lc = raw[32:32 + sz]
    segs = []
    off = 0
    symoff = nsyms = stroff = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            vmaddr, vmsize, fileoff = struct.unpack_from('<QQQ', lc, off + 24)
            segs.append((vmaddr, vmsize, fileoff))
        if cmd == 0x2:
            symoff, nsyms, stroff = struct.unpack_from('<III', lc, off + 8)
        off += csz

    def foff(addr):
        for va, vs, fo in segs:
            if va <= addr < va + vs:
                return fo + addr - va
        raise ValueError(hex(addr))
    pool_addr = pool_size = None
    if a.pool_sym:
        syms = []
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', bytes(data[symoff + 16 * i:symoff + 16 * i + 16]))
            name = bytes(data[stroff + strx:stroff + strx + 128]).split(b'\0')[0].decode()
            syms.append((nval, name))
        syms.sort()
        for k, (val, name) in enumerate(syms):
            if name == a.pool_sym:
                pool_addr = val
                pool_size = syms[k + 1][0] - val if k + 1 < len(syms) else 0x1000
        assert pool_addr, 'pool symbol not found'
    else:
        # real executables: the zero padding between the last __TEXT section and the __TEXT segment end
        t = text_seg(lc, ncmds)
        assert t, 'no __TEXT'
        start, end, segend = t
        s0 = (end + 63) & ~63
        fo = foff(s0)
        v = np.asarray(data[fo:fo + max(0, segend - s0)])
        nz = np.flatnonzero(v)
        if len(nz):
            s0 = (s0 + int(nz[-1]) + 64) & ~63
        pool_addr, pool_size = s0, max(0, segend - s0)
        print(f'pool {pool_addr:#x} +{pool_size:#x}')
        if pool_size < 256:
            print(a.path, 'no room in __TEXT')
            return
    pool = Pool([[pool_addr, pool_addr + pool_size]])
    bodies = None if a.no_thunks else Bodies(pool)

    def write_at(addr, d):
        o = foff(addr)
        data[o:o + len(d)] = np.frombuffer(d, dtype=np.uint8)
    stats = {}
    for addr, size, fo in text_sections(lc, ncmds):
        lo, hi = addr, addr + size
        # never rewrite inside the pool itself
        w = data[fo:fo + (size & ~3)].view(np.uint32) if not a.dry_run else np.array(data[fo:fo + (size & ~3)]).view(np.uint32)
        if lo <= pool_addr < hi:
            k0 = (pool_addr - addr) // 4
            k1 = min(len(w), (pool_addr + pool_size - addr) // 4)
            rewrite_section(w[:k0], addr, pool, write_at, stats, log, a.dry_run, bodies)
            rewrite_section(w[k1:], addr + 4 * k1, pool, write_at, stats, log, a.dry_run, bodies)
        else:
            rewrite_section(w, addr, pool, write_at, stats, log, a.dry_run, bodies)
    if not a.dry_run:
        data.flush()
    print(a.path, stats)


if __name__ == '__main__':
    main()
