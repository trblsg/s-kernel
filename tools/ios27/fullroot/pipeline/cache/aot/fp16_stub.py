#!/usr/bin/env python3
"""fp16_stub.py -- ARMv8.0-only out-of-line stubs for FEAT_FP16 (half-precision arithmetic) instructions.

PRIVATE (ios27-rewrite-all).  Target: Exynos M3 (ARMv8.0: has FCVT H<->S/D, H loads/stores, all S/D arithmetic,
but NO FEAT_FP16 arithmetic).  Strategy: widen half operands exactly to binary64 (FCVT D,H), compute in binary64
with the live FPCR, narrow once with FCVT H,D (honours FPCR.RMode, raises the half-format OFC/UFC/IXC).  NaN
selection, FZ16 flush-to-zero (inputs and outputs), IDC, int16 saturation etc. that the S/D datapath cannot give
are done with flag-less integer code.  See NOTES-fp16.md.

API
    is_fp16(w)                -> bool        census predicate: w is a FEAT_FP16 arithmetic-class word (the ones the
                                              M3 cannot decode; FHM/FCMA/BF16 etc are NOT included)
    stub_words_fp16(w)        -> list[int]|None   ARMv8.0 stub body (ends in RET) or None if not handled
    stub_words_fp16(w, own_frame=True) forces the stub to lower SP itself instead of using the thunk's 144 bytes.

Stub ABI (as agreed with the thunk): entered by BL; thunk already saved x30 and lowered SP by 144 -> [SP,SP+144)
is scratch.  The stub preserves every GPR/V register/NZCV/SP except the documented destination, and merges its
FPSR cumulative flags into the live FPSR.  If a stub needs more than 144 bytes it does SUB SP/ADD SP itself.
"""
import sys

M32 = 0xFFFFFFFF
THUNK_FRAME = 144
# Apple M1 (FEAT_FP16) does NOT set FPSR.IDC when FPCR.FZ16 flushes a half-precision input (verified: oracle).
# Set True to emulate the architectural-pseudocode behaviour instead.
IDC_ON_FZ16 = False


# ------------------------------------------------------------------ logical immediates
def _logical_imm(val, sf):
    size = 64 if sf else 32
    mask = (1 << size) - 1
    val &= mask
    if val == 0 or val == mask:
        return None
    e = size
    while e > 2:
        h = e >> 1
        if (val & ((1 << h) - 1)) == ((val >> h) & ((1 << h) - 1)):
            e = h
        else:
            break
    em = (1 << e) - 1
    v = val & em
    ones = bin(v).count('1')
    base = (1 << ones) - 1
    immr = None
    for r in range(e):
        ror = ((base >> r) | (base << (e - r))) & em
        if ror == v:
            immr = r
            break
    if immr is None:
        return None
    N = 1 if e == 64 else 0
    imms = ((64 - 2 * e) & 0x3f) | (ones - 1)
    return (N << 12) | (immr << 6) | imms


# ------------------------------------------------------------------ mini assembler
class Asm:
    def __init__(s):
        s.w = []
        s.lab = {}
        s.fix = []
        s.n = 0

    def emit(s, x):
        s.w.append(x & M32)

    def newlabel(s):
        s.n += 1
        return 'L%d' % s.n

    def place(s, l):
        assert l not in s.lab
        s.lab[l] = len(s.w)

    def _br(s, kind, base, label, shift=0):
        s.fix.append((len(s.w), kind, label))
        s.emit(base)

    def resolve(s):
        for pos, kind, label in s.fix:
            off = s.lab[label] - pos
            if kind == 'b':
                assert -(1 << 25) <= off < (1 << 25)
                s.w[pos] |= off & 0x3ffffff
            elif kind in ('cb', 'bc'):
                assert -(1 << 18) <= off < (1 << 18)
                s.w[pos] |= (off & 0x7ffff) << 5
            elif kind == 'tb':
                assert -(1 << 13) <= off < (1 << 13), off
                s.w[pos] |= (off & 0x3fff) << 5
        s.fix = []
        return s.w

    # ---- control flow
    def b(s, l): s._br('b', 0x14000000, l)
    def cbz(s, rt, l, sf=0): s._br('cb', (sf << 31) | 0x34000000 | rt, l)
    def cbnz(s, rt, l, sf=0): s._br('cb', (sf << 31) | 0x35000000 | rt, l)
    def tbz(s, rt, bit, l): s._br('tb', 0x36000000 | ((bit >> 5) << 31) | ((bit & 31) << 19) | rt, l)
    def tbnz(s, rt, bit, l): s._br('tb', 0x37000000 | ((bit >> 5) << 31) | ((bit & 31) << 19) | rt, l)
    def bcond(s, cond, l): s._br('bc', 0x54000000 | cond, l)
    def msr_nzcv(s, rt): s.emit(0xD51B4200 | rt)
    def ret(s): s.emit(0xD65F03C0)

    # ---- integer
    def movz(s, rd, imm, sf=0, hw=0): s.emit((sf << 31) | 0x52800000 | (hw << 21) | (imm << 5) | rd)
    def _logi(s, opc, rd, rn, val, sf):
        enc = _logical_imm(val, sf)
        assert enc is not None, hex(val)
        s.emit((sf << 31) | (opc << 29) | 0x12000000 | (enc << 10) | (rn << 5) | rd)
    def and_imm(s, rd, rn, val, sf=0): s._logi(0, rd, rn, val, sf)
    def orr_imm(s, rd, rn, val, sf=0): s._logi(1, rd, rn, val, sf)
    def eor_imm(s, rd, rn, val, sf=0): s._logi(2, rd, rn, val, sf)
    def orr_r(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x2A000000 | (rm << 16) | (rn << 5) | rd)
    def mov_r(s, rd, rm, sf=0): s.orr_r(rd, 31, rm, sf)
    def add_i(s, rd, rn, imm, sf=1): s.emit((sf << 31) | 0x11000000 | (imm << 10) | (rn << 5) | rd)
    def sub_i(s, rd, rn, imm, sf=1): s.emit((sf << 31) | 0x51000000 | (imm << 10) | (rn << 5) | rd)
    def ubfm(s, rd, rn, immr, imms, sf=0): s.emit((sf << 31) | 0x53000000 | (sf << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd)
    def lsr_i(s, rd, rn, sh, sf=0): s.ubfm(rd, rn, sh, 63 if sf else 31, sf)
    def lsl_i(s, rd, rn, sh, sf=0):
        size = 64 if sf else 32
        s.ubfm(rd, rn, (size - sh) % size, size - 1 - sh, sf)
    def sxth(s, rd, rn): s.emit(0x13003C00 | (rn << 5) | rd)
    def clz(s, rd, rn, sf=0): s.emit((sf << 31) | 0x5AC01000 | (rn << 5) | rd)
    def movn(s, rd, imm, sf=0, hw=0): s.emit((sf << 31) | 0x12800000 | (hw << 21) | (imm << 5) | rd)
    def udiv(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x1AC00800 | (rm << 16) | (rn << 5) | rd)
    def mul(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x1B007C00 | (rm << 16) | (rn << 5) | rd)
    def add_r(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x0B000000 | (rm << 16) | (rn << 5) | rd)
    def sub_r(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x4B000000 | (rm << 16) | (rn << 5) | rd)
    def lslv(s, rd, rn, rm, sf=0): s.emit((sf << 31) | 0x1AC02000 | (rm << 16) | (rn << 5) | rd)
    def asr_i(s, rd, rn, sh, sf=0): s.ubfm_s(rd, rn, sh, 63 if sf else 31, sf)
    def ubfm_s(s, rd, rn, immr, imms, sf=0): s.emit((sf << 31) | 0x13000000 | (sf << 22) | (immr << 16) | (imms << 10) | (rn << 5) | rd)
    def scvtf_dw(s, dd, wn): s.emit(0x1E620000 | (wn << 5) | dd)
    def mrs_fpcr(s, rt): s.emit(0xD53B4400 | rt)
    def mrs_fpsr(s, rt): s.emit(0xD53B4420 | rt)
    def msr_fpcr(s, rt): s.emit(0xD51B4400 | rt)
    def msr_fpsr(s, rt): s.emit(0xD51B4420 | rt)

    # ---- loads / stores (sp-relative etc.)
    def stp(s, r1, r2, rn, off): s.emit(0xA9000000 | (((off // 8) & 0x7f) << 15) | (r2 << 10) | (rn << 5) | r1)
    def ldp(s, r1, r2, rn, off): s.emit(0xA9400000 | (((off // 8) & 0x7f) << 15) | (r2 << 10) | (rn << 5) | r1)
    def str_x(s, rt, rn, off): s.emit(0xF9000000 | ((off // 8) << 10) | (rn << 5) | rt)
    def ldr_x(s, rt, rn, off): s.emit(0xF9400000 | ((off // 8) << 10) | (rn << 5) | rt)
    def ldrh(s, rt, rn, off): s.emit(0x79400000 | ((off // 2) << 10) | (rn << 5) | rt)
    def ldrsh(s, rt, rn, off): s.emit(0x79C00000 | ((off // 2) << 10) | (rn << 5) | rt)
    def strh(s, rt, rn, off): s.emit(0x79000000 | ((off // 2) << 10) | (rn << 5) | rt)
    def str_q(s, rt, rn, off): s.emit(0x3D800000 | ((off // 16) << 10) | (rn << 5) | rt)
    def ldr_q(s, rt, rn, off): s.emit(0x3DC00000 | ((off // 16) << 10) | (rn << 5) | rt)
    def str_d(s, rt, rn, off): s.emit(0xFD000000 | ((off // 8) << 10) | (rn << 5) | rt)
    def ldr_d(s, rt, rn, off): s.emit(0xFD400000 | ((off // 8) << 10) | (rn << 5) | rt)

    # ---- scalar FP / NEON moves
    def fcvt_d_h(s, rd, rn): s.emit(0x1EE2C000 | (rn << 5) | rd)
    def fcvt_h_d(s, rd, rn): s.emit(0x1E63C000 | (rn << 5) | rd)
    def fmov_s_w(s, rd, rn): s.emit(0x1E270000 | (rn << 5) | rd)
    def fmov_w_s(s, rd, rn): s.emit(0x1E260000 | (rn << 5) | rd)
    def fmov_d_x(s, rd, rn): s.emit(0x9E670000 | (rn << 5) | rd)
    def fmov_x_d(s, rd, rn): s.emit(0x9E660000 | (rn << 5) | rd)
    def fmov_s_s(s, rd, rn): s.emit(0x1E204000 | (rn << 5) | rd)
    def fmov_d_imm(s, rd, imm8): s.emit(0x1E601000 | (imm8 << 13) | rd)
    def umov_h(s, rd, vn, idx): s.emit(0x0E003C00 | (((idx << 2) | 2) << 16) | (vn << 5) | rd)
    def ins_h_w(s, vd, idx, wn): s.emit(0x4E001C00 | (((idx << 2) | 2) << 16) | (wn << 5) | vd)
    def dup_h(s, vd, vn, idx): s.emit(0x5E000400 | (((idx << 2) | 2) << 16) | (vn << 5) | vd)   # scalar DUP Hd, Vn.H[idx]
    def movi_2d_0(s, vd): s.emit(0x6F00E400 | vd)
    def movi_2s_lsl8(s, vd, imm8): s.emit(0x0F002400 | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5) | vd)
    def orr_2s_imm(s, vd, imm8): s.emit(0x0F001400 | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5) | vd)
    def movi_h_lsl8(s, vd, imm8, q): s.emit(0x0F00A400 | (q << 30) | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5) | vd)
    def orr_h_imm(s, vd, imm8, q): s.emit(0x0F009400 | (q << 30) | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5) | vd)
    def fcsel_s(s, rd, rn, rm, cond): s.emit(0x1E200C00 | (rm << 16) | (cond << 12) | (rn << 5) | rd)
    def rev32(s, vd, vn, q): s.emit((0x6E600800 if q else 0x2E600800) | (vn << 5) | vd)
    def fneg_s_vec(s, vd, vn, q): s.emit((0x6EA0F800 if q else 0x2EA0F800) | (vn << 5) | vd)
    def fneg_s(s, rd, rn): s.emit(0x1E214000 | (rn << 5) | rd)
    def fabs_s(s, rd, rn): s.emit(0x1E20C000 | (rn << 5) | rd)
    def fabs_s_vec(s, vd, vn, q): s.emit((0x4EA0F800 if q else 0x0EA0F800) | (vn << 5) | vd)

    # ---- double arithmetic
    D2 = {'mul': 0x1E600800, 'div': 0x1E601800, 'add': 0x1E602800, 'sub': 0x1E603800, 'max': 0x1E604800,
          'min': 0x1E605800, 'maxnm': 0x1E606800, 'minnm': 0x1E607800, 'nmul': 0x1E608800}
    def d2(s, op, rd, rn, rm): s.emit(Asm.D2[op] | (rm << 16) | (rn << 5) | rd)
    D1 = {'mov': 0x1E604000, 'abs': 0x1E60C000, 'neg': 0x1E614000, 'sqrt': 0x1E61C000, 'rintn': 0x1E644000,
          'rintp': 0x1E64C000, 'rintm': 0x1E654000, 'rintz': 0x1E65C000, 'rinta': 0x1E664000, 'rintx': 0x1E674000,
          'rinti': 0x1E67C000}
    def d1(s, op, rd, rn): s.emit(Asm.D1[op] | (rn << 5) | rd)
    D3 = {'fmadd': 0x1F400000, 'fmsub': 0x1F408000, 'fnmadd': 0x1F600000, 'fnmsub': 0x1F608000}
    def d3(s, op, rd, rn, rm, ra): s.emit(Asm.D3[op] | (rm << 16) | (ra << 10) | (rn << 5) | rd)
    def fccmp_d(s, rn, rm, nzcv, cond, e=False): s.emit(0x1E600400 | (rm << 16) | (cond << 12) | (rn << 5) | ((1 if e else 0) << 4) | nzcv)
    # vector-form scalar compares on D registers (result all-ones/zero in the D register)
    CMPD = {'cmeq': 0x5E60E400, 'cmge': 0x7E60E400, 'cmgt': 0x7EE0E400, 'acge': 0x7E60EC00, 'acgt': 0x7EE0EC00}
    CMPZ = {'cmeq0': 0x5EE0D800, 'cmgt0': 0x5EE0C800, 'cmge0': 0x7EE0C800, 'cmle0': 0x7EE0D800, 'cmlt0': 0x5EE0E800}
    def cmpd(s, op, rd, rn, rm=None):
        if op in Asm.CMPD:
            s.emit(Asm.CMPD[op] | (rm << 16) | (rn << 5) | rd)
        else:
            s.emit(Asm.CMPZ[op] | (rn << 5) | rd)
    # conversions D <-> int
    F2I = {'ns': 0x1E600000, 'nu': 0x1E610000, 'as': 0x1E640000, 'au': 0x1E650000, 'ps': 0x1E680000,
           'pu': 0x1E690000, 'ms': 0x1E700000, 'mu': 0x1E710000, 'zs': 0x1E780000, 'zu': 0x1E790000}
    def fcvt_int(s, kind, rd, dn, sf=0):    # FCVTxx Wd/Xd, Dn
        s.emit((sf << 31) | Asm.F2I[kind] | (dn << 5) | rd)
    def fcvt_fix(s, kind, rd, dn, fbits, sf=0):   # FCVTZS/ZU Wd/Xd, Dn, #fbits
        base = {'zs': 0x1E580000, 'zu': 0x1E590000}[kind]
        s.emit((sf << 31) | base | (((64 - fbits) & 0x3f) << 10) | (dn << 5) | rd)
    def cvtf_int(s, signed, dd, rn, sf=0):  # SCVTF/UCVTF Dd, Wn/Xn
        s.emit((sf << 31) | (0x1E620000 if signed else 0x1E630000) | (rn << 5) | dd)
    def cvtf_fix(s, signed, dd, rn, fbits, sf=0):
        s.emit((sf << 31) | (0x1E420000 if signed else 0x1E430000) | (((64 - fbits) & 0x3f) << 10) | (rn << 5) | dd)


def _fcmp_enc(rn, rm, e, zero):
    return 0x1E602000 | (rm << 16) | (rn << 5) | ((2 if e else 0) << 3) | ((1 if zero else 0) << 3)


def _fcmp_d(s, rn, rm, e=False, zero=False):
    if zero:
        s.emit(0x1E602000 | (rn << 5) | (0x18 if e else 0x08))
    else:
        s.emit(0x1E602000 | (rm << 16) | (rn << 5) | (0x10 if e else 0x00))


Asm.fcmp_d = _fcmp_d


# ------------------------------------------------------------------ decoding
THREE = {  # (U, a, opcode3) -> name   (vector three-same FP16)
    (0, 0, 0): 'maxnm', (0, 0, 1): 'fmla', (0, 0, 2): 'add', (0, 0, 3): 'mulx', (0, 0, 4): 'cmeq', (0, 0, 6): 'max',
    (0, 0, 7): 'recps', (0, 1, 0): 'minnm', (0, 1, 1): 'fmls', (0, 1, 2): 'sub', (0, 1, 6): 'min', (0, 1, 7): 'rsqrts',
    (1, 0, 0): 'maxnmp', (1, 0, 2): 'addp', (1, 0, 3): 'mul', (1, 0, 4): 'cmge', (1, 0, 5): 'acge', (1, 0, 6): 'maxp',
    (1, 0, 7): 'div', (1, 1, 0): 'minnmp', (1, 1, 2): 'abd', (1, 1, 4): 'cmgt', (1, 1, 5): 'acgt', (1, 1, 6): 'minp'}
SC_THREE = {(0, 0, 3): 'mulx', (0, 0, 4): 'cmeq', (0, 0, 7): 'recps', (0, 1, 7): 'rsqrts', (1, 0, 4): 'cmge',
            (1, 0, 5): 'acge', (1, 1, 2): 'abd', (1, 1, 4): 'cmgt', (1, 1, 5): 'acgt'}
MISC = {  # (U, a, opcode5) -> name  (two-reg misc FP16; vector & scalar)
    (0, 0, 0x18): 'rintn', (0, 0, 0x19): 'rintm', (0, 0, 0x1a): 'cvtns', (0, 0, 0x1b): 'cvtms', (0, 0, 0x1c): 'cvtas',
    (0, 0, 0x1d): 'scvtf', (0, 1, 0x0c): 'cmgt0', (0, 1, 0x0d): 'cmeq0', (0, 1, 0x0e): 'cmlt0', (0, 1, 0x0f): 'abs',
    (0, 1, 0x18): 'rintp', (0, 1, 0x19): 'rintz', (0, 1, 0x1a): 'cvtps', (0, 1, 0x1b): 'cvtzs', (0, 1, 0x1d): 'recpe',
    (0, 1, 0x1f): 'recpx',
    (1, 0, 0x18): 'rinta', (1, 0, 0x19): 'rintx', (1, 0, 0x1a): 'cvtnu', (1, 0, 0x1b): 'cvtmu', (1, 0, 0x1c): 'cvtau',
    (1, 0, 0x1d): 'ucvtf', (1, 1, 0x0c): 'cmge0', (1, 1, 0x0d): 'cmle0', (1, 1, 0x0f): 'neg', (1, 1, 0x19): 'rinti',
    (1, 1, 0x1a): 'cvtpu', (1, 1, 0x1b): 'cvtzu', (1, 1, 0x1d): 'rsqrte', (1, 1, 0x1f): 'sqrt'}
PAIRV = {(0, 0, 0x0c): 'maxnm', (0, 0, 0x0f): 'max', (0, 1, 0x0c): 'minnm', (0, 1, 0x0f): 'min'}  # across lanes
PAIRS = {(0, 0, 0x0c): 'maxnm', (0, 0, 0x0d): 'add', (0, 0, 0x0f): 'max', (0, 1, 0x0c): 'minnm', (0, 1, 0x0f): 'min'}
ELEM = {(0, 1): 'fmla', (0, 5): 'fmls', (0, 9): 'mul', (1, 9): 'mulx'}
FP1 = {0: 'mov', 1: 'abs', 2: 'neg', 3: 'sqrt', 8: 'rintn', 9: 'rintp', 10: 'rintm', 11: 'rintz', 12: 'rinta',
       14: 'rintx', 15: 'rinti'}
FP2 = {0: 'mul', 1: 'div', 2: 'add', 3: 'sub', 4: 'max', 5: 'min', 6: 'maxnm', 7: 'minnm', 8: 'nmul'}
INTCV = {0x00: 'cvtns', 0x01: 'cvtnu', 0x02: 'scvtf', 0x03: 'ucvtf', 0x04: 'cvtas', 0x05: 'cvtau', 0x06: 'fmov_wh',
         0x07: 'fmov_hw', 0x08: 'cvtps', 0x09: 'cvtpu', 0x10: 'cvtms', 0x11: 'cvtmu', 0x18: 'cvtzs', 0x19: 'cvtzu'}


def decode(w):
    """Return a dict describing an FP16 word, or None.  d['pass']=True for words the M3 can already run (FCVT h<->s/d)."""
    w &= M32
    rd, rn, rm, ra = w & 31, (w >> 5) & 31, (w >> 16) & 31, (w >> 10) & 31
    # ---------------- scalar floating point, ftype=11
    if (w & 0x7F000000) in (0x1E000000, 0x1F000000) and ((w >> 22) & 3) == 3:
        sf = w >> 31
        if (w & 0xFFE00C00) == 0x1EE00800:
            return dict(form='fp2', name=FP2.get((w >> 12) & 15), rd=rd, rn=rn, rm=rm) if (w >> 12) & 15 in FP2 else None
        if (w & 0xFFE00C00) == 0x1EE00C00:
            return dict(form='fcsel', rd=rd, rn=rn, rm=rm, cond=(w >> 12) & 15)
        if (w & 0xFFE00C00) == 0x1EE00400:
            return dict(form='fccmp', rn=rn, rm=rm, cond=(w >> 12) & 15, e=(w >> 4) & 1, nzcv=w & 15)
        if (w & 0xFFE0FC07) == 0x1EE02000:
            opc = (w >> 3) & 3
            return dict(form='fcmp', rn=rn, rm=rm, e=opc >> 1, zero=opc & 1)
        if (w & 0xFFE07C00) == 0x1EE04000:
            opc = (w >> 15) & 0x3f
            if opc in (4, 5):
                return dict(form='pass')
            if opc in FP1:
                return dict(form='fp1', name=FP1[opc], rd=rd, rn=rn)
            return None
        if (w & 0xFFE01FE0) == 0x1EE01000:
            return dict(form='fmov_imm', rd=rd, imm8=(w >> 13) & 0xff)
        if (w & 0x7FE0FC00) == 0x1EE00000:
            k = INTCV.get((w >> 16) & 0x1f)
            if k is None:
                return None
            return dict(form='intcv', name=k, sf=sf, rd=rd, rn=rn)
        if (w & 0x7FE00000) == 0x1EC00000:
            k = (w >> 16) & 0x1f
            scale = (w >> 10) & 0x3f
            if k not in (0x02, 0x03, 0x18, 0x19) or (sf == 0 and scale < 32):
                return None
            return dict(form='fixcv', name=INTCV[k], sf=sf, rd=rd, rn=rn, fbits=64 - scale)
        if (w & 0xFFC00000) == 0x1FC00000:
            o1, o0 = (w >> 21) & 1, (w >> 15) & 1
            return dict(form='fp3', name=('fmadd', 'fmsub', 'fnmadd', 'fnmsub')[o1 * 2 + o0], rd=rd, rn=rn, rm=rm, ra=ra)
        return None
    # ---------------- AdvSIMD scalar
    if (w & 0xDF60C400) == 0x5E400400:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 11) & 7)
        if k in SC_THREE:
            return dict(form='s3', name=SC_THREE[k], rd=rd, rn=rn, rm=rm, lanes=1)
        return None
    if (w & 0xDF7E0C00) == 0x5E780800:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 12) & 0x1f)
        nm = MISC.get(k)
        if nm is None or nm in ('sqrt', 'abs', 'neg') or nm.startswith('rint'):
            return None     # (no scalar-SIMD FRINT*/FABS/FNEG/FSQRT encodings exist)
        return dict(form='s2', name=nm, rd=rd, rn=rn, lanes=1)
    if (w & 0xDF7E0C00) == 0x5E300800:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 12) & 0x1f)
        if k in PAIRS:
            return dict(form='sP', name=PAIRS[k], rd=rd, rn=rn, lanes=1)
        return None
    if (w & 0xDFC00400) == 0x5F000000:
        k = ((w >> 29) & 1, (w >> 12) & 15)
        if k in ELEM:
            idx = (((w >> 11) & 1) << 2) | (((w >> 21) & 1) << 1) | ((w >> 20) & 1)
            return dict(form='sidx', name=ELEM[k], rd=rd, rn=rn, rm=(w >> 16) & 15, idx=idx, lanes=1)
        return None
    if (w & 0xDF800400) == 0x5F000400 and ((w >> 19) & 15) in (2, 3):
        k = ((w >> 29) & 1, (w >> 11) & 0x1f)
        immhb = (w >> 16) & 0x7f
        nm = {(0, 0x1c): 'scvtf', (1, 0x1c): 'ucvtf', (0, 0x1f): 'cvtzs', (1, 0x1f): 'cvtzu'}.get(k)
        if nm:
            return dict(form='s2', name=nm, rd=rd, rn=rn, lanes=1, fbits=32 - immhb)
        return None
    # ---------------- AdvSIMD vector
    q = (w >> 30) & 1
    if (w & 0xBFF8FC00) == 0x0F00FC00:          # FMOV Vd.4H/8H, #imm8 (FEAT_FP16 modified immediate)
        return dict(form='vfmov', rd=rd, imm8=(((w >> 16) & 7) << 5) | ((w >> 5) & 31), q=q)
    lanes = 8 if q else 4
    if (w & 0x9F60C400) == 0x0E400400:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 11) & 7)
        if k in THREE:
            nm = THREE[k]
            if nm in ('maxnmp', 'addp', 'maxp', 'minnmp', 'minp'):
                return dict(form='vP', name={'maxnmp': 'maxnm', 'addp': 'add', 'maxp': 'max', 'minnmp': 'minnm', 'minp': 'min'}[nm],
                            rd=rd, rn=rn, rm=rm, q=q, lanes=lanes)
            return dict(form='v3', name=nm, rd=rd, rn=rn, rm=rm, q=q, lanes=lanes)
        return None
    if (w & 0x9F7E0C00) == 0x0E780800:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 12) & 0x1f)
        nm = MISC.get(k)
        if nm is None or nm == 'recpx':
            return None
        return dict(form='v2', name=nm, rd=rd, rn=rn, q=q, lanes=lanes)
    if (w & 0x9F7E0C00) == 0x0E300800:
        k = ((w >> 29) & 1, (w >> 23) & 1, (w >> 12) & 0x1f)
        if k in PAIRV:
            return dict(form='vA', name=PAIRV[k], rd=rd, rn=rn, q=q, lanes=lanes)
        return None
    if (w & 0x9FC00400) == 0x0F000000:
        k = ((w >> 29) & 1, (w >> 12) & 15)
        if k in ELEM:
            idx = (((w >> 11) & 1) << 2) | (((w >> 21) & 1) << 1) | ((w >> 20) & 1)
            return dict(form='vidx', name=ELEM[k], rd=rd, rn=rn, rm=(w >> 16) & 15, idx=idx, q=q, lanes=lanes)
        return None
    if (w & 0x9F800400) == 0x0F000400 and ((w >> 19) & 15) in (2, 3):
        k = ((w >> 29) & 1, (w >> 11) & 0x1f)
        immhb = (w >> 16) & 0x7f
        nm = {(0, 0x1c): 'scvtf', (1, 0x1c): 'ucvtf', (0, 0x1f): 'cvtzs', (1, 0x1f): 'cvtzu'}.get(k)
        if nm:
            return dict(form='v2', name=nm, rd=rd, rn=rn, q=q, lanes=lanes, fbits=32 - immhb)
        return None
    return None


def is_fp16(w):
    """Census predicate: True iff w is an FP16-arithmetic-class word (everything decode() recognises except the
    FCVT H<->S/D conversions which the M3 already executes natively)."""
    d = decode(w)
    return d is not None and d.get('form') != 'pass'


# ------------------------------------------------------------------ stub generator
class Gen:
    def __init__(s, roles, excl_g=(), excl_v=(), nd=3, narr=0, own_frame=False):
        s.a = Asm()
        pool = [r for r in range(18) if r not in excl_g]
        roles = list(roles)
        assert len(pool) >= len(roles)
        s.roles = roles
        s.reg = {}
        for n, r in zip(roles, pool):
            s.reg[n] = r
            setattr(s, n, r)
        s.saved_g = [s.reg[n] for n in roles]
        vpool = [r for r in range(31, 15, -1) if r not in excl_v]
        s.D = vpool[:nd]
        s.nd = nd
        s.vbytes = 16 * nd
        s.gbytes = 8 * len(s.saved_g)
        s.arr_off = s.vbytes + s.gbytes
        s.arr_off = (s.arr_off + 15) & ~15
        s.FS = (s.arr_off + 16 * narr + 15) & ~15
        s.own = own_frame or s.FS > THUNK_FRAME
        s.lcount = 0

    def lab(s):
        return s.a.newlabel()

    def _gsave(s, save):
        a = s.a
        n = len(s.saved_g)
        for k in range(0, n - 1, 2):
            (a.stp if save else a.ldp)(s.saved_g[k], s.saved_g[k + 1], 31, s.vbytes + 8 * k)
        if n & 1:
            (a.str_x if save else a.ldr_x)(s.saved_g[-1], 31, s.vbytes + 8 * (n - 1))

    def prologue(s):
        a = s.a
        if s.own:
            a.sub_i(31, 31, s.FS)
        for i, r in enumerate(s.D):
            a.str_q(r, 31, 16 * i)
        s._gsave(True)
        a.mrs_fpsr(s.S)
        a.msr_fpsr(31)
        a.mrs_fpcr(s.F)
        # FEAT_FP16 arithmetic always uses IEEE half, but FCVT H<->D honours FPCR.AHP: run with AHP=0 (restored at exit)
        Lok = s.lab()
        a.tbz(s.F, 26, Lok)
        a.and_imm(s.T, s.F, 0xfbffffff)
        a.msr_fpcr(s.T)
        a.place(Lok)

    def epilogue(s):
        a = s.a
        a.mrs_fpsr(s.T)
        a.orr_r(s.S, s.S, s.T, 1)
        a.msr_fpsr(s.S)
        s.restore()

    def restore(s):
        a = s.a
        Lok = s.lab()
        a.tbz(s.F, 26, Lok)
        a.msr_fpcr(s.F)
        a.place(Lok)
        s._gsave(False)
        for i, r in enumerate(s.D):
            a.ldr_q(r, 31, 16 * i)
        if s.own:
            a.add_i(31, 31, s.FS)
        a.ret()

    def words(s):
        return s.a.resolve()


# ---- reusable code fragments (all flag-less: no NZCV clobber) -----------------
def fz_begin(g, regs, isolate):
    """If FPCR.FZ16: flush subnormal half operands to signed zero (+IDC into S); optionally isolate FPSR."""
    a = g.a
    Ls = g.lab()
    a.tbz(g.F, 19, Ls)
    for x in regs:
        Lk = g.lab()
        a.and_imm(g.T, x, 0x7c00)
        a.cbnz(g.T, Lk)
        a.and_imm(g.T, x, 0x3ff)
        a.cbz(g.T, Lk)
        a.and_imm(x, x, 0x8000)
        if IDC_ON_FZ16:
            a.orr_imm(g.S, g.S, 0x80, 1)
        a.place(Lk)
    if isolate:
        a.mrs_fpsr(g.T)
        a.orr_r(g.S, g.S, g.T, 1)
        a.msr_fpsr(31)
    a.place(Ls)


def nan_any(g, regs, Ln):
    a = g.a
    for x in regs:
        Lnot = g.lab()
        a.and_imm(g.T, x, 0x7c00)
        a.eor_imm(g.T, g.T, 0x7c00)
        a.cbnz(g.T, Lnot)
        a.and_imm(g.T, x, 0x3ff)
        a.cbnz(g.T, Ln)
        a.place(Lnot)


def is_snan_or(g, x, Lno):
    """falls through if x is a signalling NaN, else branches to Lno"""
    a = g.a
    a.and_imm(g.T, x, 0x7e00)
    a.eor_imm(g.T, g.T, 0x7c00)
    a.cbnz(g.T, Lno)
    a.and_imm(g.T, x, 0x1ff)
    a.cbz(g.T, Lno)


def is_qnan_or(g, x, Lno):
    a = g.a
    a.and_imm(g.T, x, 0x7e00)
    a.eor_imm(g.T, g.T, 0x7e00)
    a.cbnz(g.T, Lno)


def nan_select(g, order, neg=(), quirk=None):
    """At least one operand of `order` is a NaN.  Compute FPProcessNaNs{,3}: first SNaN else first QNaN, quieted;
    IOC if any SNaN; FPCR.DN -> default NaN.  Result in g.R, falls through at the end.
    neg: indices whose sign is flipped first (FPNeg of FMSUB etc).  quirk=(n,m,acc): FMA QNaN-addend inf*0 rule."""
    a = g.a
    R = g.R
    for i in neg:
        a.eor_imm(order[i], order[i], 0x8000)
    Lend = g.lab()
    if quirk:
        n, m, acc = quirk
        Lno = g.lab()
        Linv = g.lab()
        Lnz = g.lab()
        is_qnan_or(g, acc, Lno)
        a.and_imm(g.T, n, 0x7fff)
        a.and_imm(g.U, m, 0x7fff)
        a.cbz(g.T, Lnz)
        a.eor_imm(g.T, g.T, 0x7c00)
        a.cbnz(g.T, Lno)
        a.cbnz(g.U, Lno)
        a.b(Linv)
        a.place(Lnz)
        a.eor_imm(g.U, g.U, 0x7c00)
        a.cbnz(g.U, Lno)
        a.place(Linv)
        a.movz(R, 0x7e00)
        a.orr_imm(g.S, g.S, 1, 1)
        a.b(Lend)
        a.place(Lno)
    Lq = g.lab()
    for x in order:
        Lnx = g.lab()
        is_snan_or(g, x, Lnx)
        a.mov_r(R, x)
        a.orr_imm(g.S, g.S, 1, 1)
        a.b(Lq)
        a.place(Lnx)
    for x in order:
        Lnx = g.lab()
        is_qnan_or(g, x, Lnx)
        a.mov_r(R, x)
        a.b(Lq)
        a.place(Lnx)
    a.place(Lq)
    a.orr_imm(R, R, 0x200)
    Ldn = g.lab()
    a.tbz(g.F, 25, Ldn)
    a.movz(R, 0x7e00)
    a.place(Ldn)
    a.place(Lend)


def to_d(g, dreg, wreg):
    g.a.fmov_s_w(dreg, wreg)
    g.a.fcvt_d_h(dreg, dreg)


def finish(g, d, flushout):
    """d holds the exact(ish) result.  Narrow with FCVT H,D (honours RMode) -> g.R.  FZ16: tiny results -> signed zero + UFC."""
    a = g.a
    if flushout:
        Lnf = g.lab()
        Ltiny = g.lab()
        Lend = g.lab()
        a.tbz(g.F, 19, Lnf)
        a.fmov_x_d(g.T, d)
        a.lsl_i(g.U, g.T, 1, 1)
        a.cbz(g.U, Lnf, 1)
        a.lsr_i(g.U, g.U, 53, 1)
        a.sub_i(g.U, g.U, 1009, 1)
        a.tbnz(g.U, 63, Ltiny)
        a.b(Lnf)
        a.place(Ltiny)
        a.msr_fpsr(31)
        a.movz(g.U, 8)
        a.msr_fpsr(g.U)
        a.lsr_i(g.T, g.T, 63, 1)
        a.lsl_i(g.R, g.T, 15)
        a.b(Lend)
        a.place(Lnf)
        a.fcvt_h_d(d, d)
        a.umov_h(g.R, d, 0)
        a.place(Lend)
    else:
        a.fcvt_h_d(d, d)
        a.umov_h(g.R, d, 0)


FLUSHOUT = {'add', 'sub', 'mul', 'div', 'abd', 'mulx', 'nmul'}


def core_bin(g, name, A, B):
    a = g.a
    R = g.R
    d0, d1 = g.D[0], g.D[1]
    Ld, Ln, Lf = g.lab(), g.lab(), g.lab()
    fo = name in FLUSHOUT
    fz_begin(g, [A, B], fo)
    nan_any(g, [A, B], Ln)
    if name == 'mulx':
        Lz1 = g.lab()
        Lsp = g.lab()
        a.and_imm(g.T, A, 0x7fff)
        a.and_imm(g.U, B, 0x7fff)
        a.cbz(g.T, Lz1)
        a.eor_imm(g.T, g.T, 0x7c00)
        a.cbnz(g.T, Lf)
        a.cbnz(g.U, Lf)
        a.b(Lsp)
        a.place(Lz1)
        a.eor_imm(g.U, g.U, 0x7c00)
        a.cbnz(g.U, Lf)
        a.place(Lsp)
        a.emit(0x4a000000 | (B << 16) | (A << 5) | R)     # eor wR, wA, wB
        a.and_imm(R, R, 0x8000)
        a.orr_imm(R, R, 0x4000)
        a.b(Ld)
    a.place(Lf)
    to_d(g, d0, A)
    to_d(g, d1, B)
    op = {'abd': 'sub', 'mulx': 'mul', 'nmul': 'mul'}.get(name, name)
    a.d2(op, d0, d0, d1)
    finish(g, d0, fo)
    if name == 'abd':           # FABD = FPAbs(FPSub): round the signed difference, THEN clear the sign (RP/RM!)
        a.and_imm(R, R, 0x7fff)
    if name == 'nmul':          # FNMUL = FPNeg(FPMul): round the positive product, THEN negate (matters for RP/RM)
        a.eor_imm(R, R, 0x8000)
    a.b(Ld)
    a.place(Ln)
    if name in ('maxnm', 'minnm'):
        Lstd, L1, L2, Lb, Lbq = g.lab(), g.lab(), g.lab(), g.lab(), g.lab()
        is_snan_or(g, A, L1)
        a.b(Lstd)                 # A is SNaN -> standard NaN rules
        a.place(L1)
        is_snan_or(g, B, L2)
        a.b(Lstd)                 # B is SNaN
        a.place(L2)
        # no SNaN: both QNaN -> standard; exactly one QNaN -> the other (numeric) operand
        is_qnan_or(g, A, Lb)
        is_qnan_or(g, B, Lbq)
        a.b(Lstd)                 # both quiet
        a.place(Lbq)
        a.mov_r(R, B)             # A QNaN, B number
        a.b(Ld)
        a.place(Lb)
        a.mov_r(R, A)             # B QNaN, A number
        a.b(Ld)
        a.place(Lstd)
    nan_select(g, [A, B])
    if name == 'nmul':
        a.eor_imm(R, R, 0x8000)
    if name == 'abd':
        a.and_imm(R, R, 0x7fff)
    a.place(Ld)


def core_recstep(g, name, A, B):
    """FRECPS / FRSQRTS"""
    a = g.a
    R = g.R
    d0, d1, d2 = g.D
    Ld, Ln, Lf, Lsp = g.lab(), g.lab(), g.lab(), g.lab()
    fz_begin(g, [A, B], True)
    nan_any(g, [A, B], Ln)
    Lz1 = g.lab()
    a.and_imm(g.T, A, 0x7fff)
    a.and_imm(g.U, B, 0x7fff)
    a.cbz(g.T, Lz1)
    a.eor_imm(g.T, g.T, 0x7c00)
    a.cbnz(g.T, Lf)
    a.cbnz(g.U, Lf)
    a.b(Lsp)
    a.place(Lz1)
    a.eor_imm(g.U, g.U, 0x7c00)
    a.cbnz(g.U, Lf)
    a.place(Lsp)
    a.movz(R, 0x4000 if name == 'recps' else 0x3e00)
    a.b(Ld)
    a.place(Lf)
    to_d(g, d0, A)
    to_d(g, d1, B)
    a.fmov_d_imm(d2, 0x00 if name == 'recps' else 0x08)      # 2.0 / 3.0
    a.d3('fmsub', d0, d0, d1, d2)                             # c - a*b
    if name == 'rsqrts':
        a.fmov_d_imm(d1, 0x60)                                # 0.5
        a.d2('mul', d0, d0, d1)
    finish(g, d0, True)
    a.b(Ld)
    a.place(Ln)
    nan_select(g, [A, B], neg=(0,))
    a.place(Ld)


def core_fma(g, name, N, M, C):
    a = g.a
    R = g.R
    d0, d1, d2 = g.D
    Ld, Ln = g.lab(), g.lab()
    fz_begin(g, [N, M, C], True)
    nan_any(g, [N, M, C], Ln)
    to_d(g, d0, N)
    to_d(g, d1, M)
    to_d(g, d2, C)
    a.d3(name, d0, d0, d1, d2)
    finish(g, d0, True)
    a.b(Ld)
    a.place(Ln)
    # native NaN priority (verified on M1): addend, then Rn, then Rm.  FMSUB negates Rn, FNMADD Rn+addend, FNMSUB addend.
    neg = {'fmadd': (), 'fmsub': (1,), 'fnmadd': (0, 1), 'fnmsub': (0,)}[name]
    nan_select(g, [C, N, M], neg=neg, quirk=(N, M, C))
    a.place(Ld)


def core_cmp(g, kind, A, B):
    a = g.a
    d0, d1 = g.D[0], g.D[1]
    zero = kind.endswith('0')
    fz_begin(g, [A] if zero else [A, B], False)
    to_d(g, d0, A)
    if not zero:
        to_d(g, d1, B)
    a.cmpd(kind, d0, d0, None if zero else d1)
    a.fmov_x_d(g.T, d0)
    a.and_imm(g.R, g.T, 0xffff)


def core_un(g, name, A):
    a = g.a
    d0 = g.D[0]
    fz_begin(g, [A], False)
    to_d(g, d0, A)
    a.d1(name, d0, d0)
    finish(g, d0, False)


def core_f2i16(g, name, A, fbits):
    """vector / scalar-SIMD FCVT*{S,U} half -> 16-bit int in g.R (saturating, IOC), int16 range check flag-less"""
    a = g.a
    d0 = g.D[0]
    signed = name.endswith('s')
    kind = name[3:]
    fz_begin(g, [A], False)
    to_d(g, d0, A)
    if fbits:
        a.fcvt_fix(kind, g.T, d0, fbits, 0)
    else:
        a.fcvt_int(kind, g.T, d0, 0)
    Lok, Lend = g.lab(), g.lab()
    if signed:
        a.sxth(g.U, g.T)
        a.emit(0x4a000000 | (g.T << 16) | (g.U << 5) | g.U)         # eor wU, wU, wT
        a.cbz(g.U, Lok)
        Lneg = g.lab()
        Lfl = g.lab()
        a.tbnz(g.T, 31, Lneg)
        a.movz(g.R, 0x7fff)
        a.b(Lfl)
        a.place(Lneg)
        a.movz(g.R, 0x8000)
        a.place(Lfl)
        a.orr_imm(g.S, g.S, 1, 1)
        a.b(Lend)
        a.place(Lok)
        a.and_imm(g.R, g.T, 0xffff)
        a.place(Lend)
    else:
        a.lsr_i(g.U, g.T, 16)
        a.cbz(g.U, Lok)
        a.movz(g.R, 0xffff)
        a.orr_imm(g.S, g.S, 1, 1)
        a.b(Lend)
        a.place(Lok)
        a.and_imm(g.R, g.T, 0xffff)
        a.place(Lend)


def core_i2f16(g, signed, A, fbits):
    """int16 (already loaded into A: sign/zero extended appropriately) -> half in g.R"""
    a = g.a
    d0 = g.D[0]
    fo = fbits > 0
    if fo:
        fz_begin(g, [], True)
    if fbits:
        a.cvtf_fix(signed, d0, A, fbits, 0)
    else:
        a.cvtf_int(signed, d0, A, 0)
    finish(g, d0, fo)




def _nan_unary(g, A, Lq_done):
    """A is a NaN: FPProcessNaN -> quiet, IOC if SNaN, DN -> default NaN.  result in R."""
    a = g.a
    Lq = g.lab()
    a.mov_r(g.R, A)
    a.tbnz(g.R, 9, Lq)
    a.orr_imm(g.S, g.S, 1, 1)
    a.place(Lq)
    a.orr_imm(g.R, g.R, 0x200)
    a.tbz(g.F, 25, Lq_done)
    a.movz(g.R, 0x7e00)
    a.b(Lq_done)


def _recip_prefix(g, A):
    """split A into X=sign, Y=exp, Z=frac; handle NaN / inf(for recpe handled by caller).  FZ16 flush first."""
    a = g.a
    fz_begin(g, [A], False)
    a.lsr_i(g.X, A, 15)
    a.lsr_i(g.Y, A, 10)
    a.and_imm(g.Y, g.Y, 31)
    a.and_imm(g.Z, A, 0x3ff)


def core_recpe(g, A):
    a = g.a
    R, T, U, X, Y, Z = g.R, g.T, g.U, g.X, g.Y, g.Z
    Ld, Lnn, Linf, Lnz, Lnoovf, Lcalc, Lnormal, Lb9 = (g.lab() for _ in range(8))
    _recip_prefix(g, A)
    a.eor_imm(T, Y, 31)
    a.cbnz(T, Lnn)
    a.cbz(Z, Linf)
    _nan_unary(g, A, Ld)
    a.place(Linf)
    a.lsl_i(R, X, 15)
    a.b(Ld)
    a.place(Lnn)
    a.orr_r(T, Y, Z)
    a.cbnz(T, Lnz)
    a.lsl_i(R, X, 15)
    a.orr_imm(R, R, 0x7c00)
    a.orr_imm(g.S, g.S, 2, 1)           # DZC
    a.b(Ld)
    a.place(Lnz)
    # overflow: tiny subnormal input (exp==0 && frac<256)
    a.cbnz(Y, Lnoovf)
    a.lsr_i(T, Z, 8)
    a.cbnz(T, Lnoovf)
    Linf2, Lmax, Lrp, Lfl = g.lab(), g.lab(), g.lab(), g.lab()
    a.lsr_i(T, g.F, 22, 1)
    a.and_imm(T, T, 3)
    a.cbz(T, Linf2)
    a.sub_i(U, T, 3, 0)
    a.cbz(U, Lmax)
    a.sub_i(U, T, 1, 0)
    a.cbz(U, Lrp)
    a.cbnz(X, Linf2)                    # RM: inf iff negative
    a.b(Lmax)
    a.place(Lrp)
    a.cbz(X, Linf2)                     # RP: inf iff positive
    a.b(Lmax)
    a.place(Linf2)
    a.lsl_i(R, X, 15)
    a.orr_imm(R, R, 0x7c00)
    a.b(Lfl)
    a.place(Lmax)
    a.lsl_i(R, X, 15)
    a.movz(T, 0x7bff)
    a.orr_r(R, R, T)
    a.place(Lfl)
    a.orr_imm(g.S, g.S, 4, 1)           # OFC
    a.orr_imm(g.S, g.S, 0x10, 1)        # IXC
    a.b(Ld)
    a.place(Lnoovf)
    # FZ16 && |x| >= 2^14 -> +-0, UFC
    a.tbz(g.F, 19, Lcalc)
    a.sub_i(T, Y, 29, 0)
    a.tbnz(T, 31, Lcalc)
    a.lsl_i(R, X, 15)
    a.orr_imm(g.S, g.S, 8, 1)
    a.b(Ld)
    a.place(Lcalc)
    a.cbnz(Y, Lnormal)
    a.tbnz(Z, 9, Lb9)
    a.lsl_i(Z, Z, 2)
    a.and_imm(Z, Z, 0x3ff)
    a.movn(Y, 0)                         # exp = -1
    a.b(Lnormal)
    a.place(Lb9)
    a.lsl_i(Z, Z, 1)
    a.and_imm(Z, Z, 0x3ff)
    a.place(Lnormal)
    a.lsr_i(T, Z, 2)
    a.orr_imm(T, T, 0x100)               # scaled 256..511
    a.lsl_i(T, T, 1)
    a.orr_imm(T, T, 1)                   # a*2+1
    a.movz(U, 8, 0, 1)                   # 2^19
    a.udiv(U, U, T)
    a.add_i(U, U, 1, 0)
    a.lsr_i(U, U, 1)                     # estimate
    a.and_imm(U, U, 0xff)
    a.movz(T, 29)
    a.sub_r(T, T, Y)                     # result_exp
    a.lsl_i(Z, U, 2)
    Lne0, Lass = g.lab(), g.lab()
    a.cbnz(T, Lne0)
    a.lsr_i(Z, Z, 1)
    a.orr_imm(Z, Z, 0x200)
    a.b(Lass)
    a.place(Lne0)
    a.add_i(U, T, 1, 0)
    a.cbnz(U, Lass)
    a.lsr_i(Z, Z, 2)
    a.orr_imm(Z, Z, 0x100)
    a.movz(T, 0)
    a.place(Lass)
    a.lsl_i(R, X, 15)
    a.lsl_i(U, T, 10)
    a.orr_r(R, R, U)
    a.orr_r(R, R, Z)
    a.place(Ld)


def core_rsqrte(g, A):
    a = g.a
    R, T, U, X, Y, Z, W = g.R, g.T, g.U, g.X, g.Y, g.Z, g.W
    d0, d1 = g.D[0], g.D[1]
    Ld, Lnn, Lnz, Lpos, Lnormal, Leven, Lgo = (g.lab() for _ in range(7))
    _recip_prefix(g, A)
    a.eor_imm(T, Y, 31)
    a.cbnz(T, Lnn)
    Linf = g.lab()
    a.cbz(Z, Linf)
    _nan_unary(g, A, Ld)
    a.place(Linf)
    a.cbnz(X, Lnn)                       # -inf: falls to the negative case below
    a.movz(R, 0)
    a.b(Ld)
    a.place(Lnn)
    a.orr_r(T, Y, Z)
    a.cbnz(T, Lnz)
    a.lsl_i(R, X, 15)
    a.orr_imm(R, R, 0x7c00)
    a.orr_imm(g.S, g.S, 2, 1)            # DZC
    a.b(Ld)
    a.place(Lnz)
    a.cbz(X, Lpos)
    a.movz(R, 0x7e00)                    # negative -> default NaN, IOC
    a.orr_imm(g.S, g.S, 1, 1)
    a.b(Ld)
    a.place(Lpos)
    # subnormal normalisation: Y = -n, Z = (Z << (n+1)) & 0x3ff  where n = leading zeros of the 10-bit fraction
    a.cbnz(Y, Lnormal)
    a.clz(T, Z)
    a.sub_i(T, T, 22, 0)                 # n
    a.sub_r(Y, 31, T)                    # Y = -n (wzr - n)
    a.add_i(T, T, 1, 0)
    a.lslv(Z, Z, T)
    a.and_imm(Z, Z, 0x3ff)
    a.place(Lnormal)
    Lodd, Lsc = g.lab(), g.lab()
    a.tbnz(Y, 0, Lodd)
    a.lsr_i(T, Z, 2)
    a.orr_imm(T, T, 0x100)
    a.b(Lsc)
    a.place(Lodd)
    a.lsr_i(T, Z, 3)
    a.orr_imm(T, T, 0x80)
    a.place(Lsc)
    # result_exp = (44 - Y) / 2
    a.movz(U, 44)
    a.sub_r(U, U, Y)
    a.lsr_i(U, U, 1)
    a.mov_r(X, U)                        # X <- result_exp (sign is known to be 0)
    # RecipSqrtEstimate(T)   (T = scaled 128..511)
    Lhi, Lah = g.lab(), g.lab()
    a.lsr_i(U, T, 8)
    a.cbnz(U, Lhi)
    a.lsl_i(T, T, 1)
    a.orr_imm(T, T, 1)
    a.b(Lah)
    a.place(Lhi)
    a.and_imm(T, T, 0xfffffffe)
    a.add_i(T, T, 1, 0)
    a.lsl_i(T, T, 1)
    a.place(Lah)
    # k = smallest k with T*k*k >= 2^28 ; estimate r = k >> 1.  k0 from a binary64 sqrt (FPSR protected).
    a.mrs_fpsr(W)
    a.scvtf_dw(d0, T)
    a.movz(U, 0x41b0, 1, 3)
    a.fmov_d_x(d1, U)
    a.d2('div', d0, d1, d0)
    a.d1('sqrt', d0, d0)
    a.fcvt_int('zu', U, d0, 0)           # k0 (w)
    a.msr_fpsr(W)
    a.movz(W, 0x1000, 0, 1)              # 2^28
    for _ in range(2):                   # while a*k*k < 2^28: k++
        Lskip = g.lab()
        a.mul(Y, U, U, 1)
        a.mul(Y, Y, T, 1)
        a.sub_r(Y, Y, W, 1)
        a.tbz(Y, 63, Lskip)
        a.add_i(U, U, 1, 0)
        a.place(Lskip)
    for _ in range(2):                   # while a*(k-1)^2 >= 2^28: k--
        Lskip = g.lab()
        a.sub_i(Y, U, 1, 0)
        a.mul(Y, Y, Y, 1)
        a.mul(Y, Y, T, 1)
        a.sub_r(Y, Y, W, 1)
        a.tbnz(Y, 63, Lskip)
        a.sub_i(U, U, 1, 0)
        a.place(Lskip)
    a.lsr_i(U, U, 1)
    a.and_imm(U, U, 0xff)
    a.lsl_i(U, U, 2)
    a.lsl_i(R, X, 10)
    a.orr_r(R, R, U)
    a.place(Ld)


def core_recpx(g, A):
    a = g.a
    R, T, U, X, Y, Z = g.R, g.T, g.U, g.X, g.Y, g.Z
    Ld, Lnn = g.lab(), g.lab()
    a.lsr_i(X, A, 15)
    a.lsr_i(Y, A, 10)
    a.and_imm(Y, Y, 31)
    a.and_imm(Z, A, 0x3ff)
    a.eor_imm(T, Y, 31)
    a.cbnz(T, Lnn)
    a.cbz(Z, Lnn)
    _nan_unary(g, A, Ld)
    a.place(Lnn)
    Lnz = g.lab()
    a.cbnz(Y, Lnz)
    a.movz(U, 30)
    Lfin = g.lab()
    a.b(Lfin)
    a.place(Lnz)
    a.eor_imm(U, Y, 31)
    a.place(Lfin)
    a.lsl_i(R, X, 15)
    a.lsl_i(T, U, 10)
    a.orr_r(R, R, T)
    a.place(Ld)


# ---- stub builders ---------------------------------------------------------
def _named(d):
    v = set()
    for k in ('rd', 'rn', 'rm', 'ra'):
        if k in d and d[k] is not None:
            v.add(d[k])
    return v


def _elem_core(g, d, A, B, C):
    nm = d['name']
    if nm in ('add', 'sub', 'mul', 'div', 'max', 'min', 'maxnm', 'minnm', 'abd', 'mulx', 'nmul'):
        core_bin(g, nm, A, B)
    elif nm in ('fmla', 'fmls', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub'):
        core_fma(g, {'fmla': 'fmadd', 'fmls': 'fmsub'}.get(nm, nm), A, B, C)
    elif nm in ('cmeq', 'cmge', 'cmgt', 'acge', 'acgt', 'cmeq0', 'cmge0', 'cmgt0', 'cmle0', 'cmlt0'):
        core_cmp(g, nm, A, B)
    elif nm in ('sqrt', 'rintn', 'rintp', 'rintm', 'rintz', 'rinta', 'rintx', 'rinti'):
        core_un(g, nm, A)
    elif nm in ('recps', 'rsqrts'):
        core_recstep(g, nm, A, B)
    elif nm == 'recpe':
        core_recpe(g, A)
    elif nm == 'rsqrte':
        core_rsqrte(g, A)
    elif nm == 'recpx':
        core_recpx(g, A)
    elif nm in ('cvtns', 'cvtnu', 'cvtas', 'cvtau', 'cvtps', 'cvtpu', 'cvtms', 'cvtmu', 'cvtzs', 'cvtzu'):
        core_f2i16(g, nm, A, d.get('fbits', 0))
    elif nm in ('scvtf', 'ucvtf'):
        core_i2f16(g, nm == 'scvtf', A, d.get('fbits', 0))
    else:
        raise NotImplementedError(nm)


def _build_elementwise(d, own):
    """scalar-SIMD (lanes==1) and vector (lanes 4/8) element-wise forms"""
    form = d['form']
    nm = d['name']
    lanes = d['lanes']
    rd, rn = d['rd'], d['rn']
    rm = d.get('rm')
    # operands: list of (kind, vreg[, idx])
    if form in ('s3', 'v3'):
        srcs = [('v', rn), ('v', rm)]
        if nm in ('fmla', 'fmls'):
            srcs.append(('v', rd))
    elif form in ('s2', 'v2'):
        if nm in ('cmgt0', 'cmeq0', 'cmlt0', 'cmge0', 'cmle0'):
            srcs = [('v', rn)]
        else:
            srcs = [('v', rn)]
    elif form in ('sidx', 'vidx'):
        srcs = [('v', rn), ('e', rm, d['idx'])]
        if nm in ('fmla', 'fmls'):
            srcs.append(('v', rd))
    else:
        raise NotImplementedError(form)
    intsrc = nm in ('scvtf', 'ucvtf')
    named = {rd, rn} | ({rm} if rm is not None else set())
    narr = 0 if lanes == 1 else len(srcs)
    roles = ['A', 'B', 'C', 'R', 'T', 'U', 'S', 'F'] + (['P', 'K'] if lanes > 1 else [])
    if nm in ('recpe', 'rsqrte', 'recpx'):
        roles += ['X', 'Y', 'Z', 'W']
    g = Gen(roles, excl_v=named, nd=3, narr=narr, own_frame=own)
    a = g.a
    g.prologue()
    regs = [g.A, g.B, g.C]
    if lanes == 1:
        for i, s_ in enumerate(srcs):
            idx = 0 if s_[0] == 'v' else s_[2]
            a.umov_h(regs[i], s_[1], idx)
        if intsrc and nm in ('scvtf',):
            a.sxth(g.A, g.A)
        _elem_core(g, d, g.A, g.B, g.C)
        a.fmov_s_w(rd, g.R)
    else:
        base = g.arr_off
        # copy the distinct source vectors into the frame (slot i = srcs[i]) ; result goes back to slot 0
        for i, s_ in enumerate(srcs):
            a.str_q(s_[1], 31, base + 16 * i)
        a.add_i(g.P, 31, base)
        a.movz(g.K, lanes)
        Lh = g.lab()
        a.place(Lh)
        for i, s_ in enumerate(srcs):
            if s_[0] == 'e':
                a.ldrh(regs[i], 31, base + 16 * i + 2 * s_[2])
            else:
                if i == 0 and intsrc and nm == 'scvtf':
                    a.ldrsh(regs[i], g.P, 16 * i)
                else:
                    a.ldrh(regs[i], g.P, 16 * i)
        _elem_core(g, d, g.A, g.B, g.C)
        a.strh(g.R, g.P, 0)
        a.add_i(g.P, g.P, 2)
        a.sub_i(g.K, g.K, 1)
        a.cbnz(g.K, Lh)
        if lanes == 8:
            a.ldr_q(rd, 31, base)
        else:
            a.ldr_d(rd, 31, base)
    g.epilogue()
    return g.words()


def _build_pair(d, own):
    """FADDP/FMAXP/... (vector pairwise) and scalar pairwise (FADDP h, v.2h) and across-lanes reductions"""
    form = d['form']
    nm = d['name']
    rd, rn = d['rd'], d['rn']
    rm = d.get('rm')
    lanes = d['lanes']
    named = {rd, rn} | ({rm} if rm is not None else set())
    roles = ['A', 'B', 'C', 'R', 'T', 'U', 'S', 'F', 'P', 'K', 'Q', 'N']
    g = Gen(roles, excl_v=named, nd=3, narr=2, own_frame=own)
    a = g.a
    g.prologue()
    base = g.arr_off
    dd = dict(name=nm)
    if form == 'sP':
        a.umov_h(g.A, rn, 0)
        a.umov_h(g.B, rn, 1)
        _elem_core(g, dd, g.A, g.B, g.C)
        a.fmov_s_w(rd, g.R)
    elif form == 'vP':
        if lanes == 8:
            a.str_q(rn, 31, base)
            a.str_q(rm, 31, base + 16)
        else:
            a.str_d(rn, 31, base)
            a.str_d(rm, 31, base + 8)
        a.add_i(g.P, 31, base)
        # output is written in place (reads at 2j,2j+1 precede the write at j <= 2j)
        a.add_i(g.Q, 31, base)
        a.movz(g.K, lanes)
        Lh = g.lab()
        a.place(Lh)
        a.ldrh(g.A, g.P, 0)
        a.ldrh(g.B, g.P, 2)
        _elem_core(g, dd, g.A, g.B, g.C)
        a.strh(g.R, g.Q, 0)
        a.add_i(g.P, g.P, 4)
        a.add_i(g.Q, g.Q, 2)
        a.sub_i(g.K, g.K, 1)
        a.cbnz(g.K, Lh)
        if lanes == 8:
            a.ldr_q(rd, 31, base)
        else:
            a.ldr_d(rd, 31, base)
    elif form == 'vA':
        if lanes == 8:
            a.str_q(rn, 31, base)
        else:
            a.str_d(rn, 31, base)
        a.movz(g.N, lanes)
        Lo = g.lab()
        a.place(Lo)
        a.add_i(g.P, 31, base)
        a.add_i(g.Q, 31, base)
        a.lsr_i(g.K, g.N, 1)
        Lh = g.lab()
        a.place(Lh)
        a.ldrh(g.A, g.P, 0)
        a.ldrh(g.B, g.P, 2)
        _elem_core(g, dd, g.A, g.B, g.C)
        a.strh(g.R, g.Q, 0)
        a.add_i(g.P, g.P, 4)
        a.add_i(g.Q, g.Q, 2)
        a.sub_i(g.K, g.K, 1)
        a.cbnz(g.K, Lh)
        a.lsr_i(g.N, g.N, 1)
        a.sub_i(g.U, g.N, 1)
        a.cbnz(g.U, Lo)
        a.ldrh(g.R, 31, base)
        a.fmov_s_w(rd, g.R)
    g.epilogue()
    return g.words()


def _build_fp_scalar(d, own):
    form = d['form']
    nm = d.get('name')
    named = {d.get(k) for k in ('rd', 'rn', 'rm', 'ra') if d.get(k) is not None}
    if form in ('fp2', 'fp3', 'fp1'):
        n3 = form == 'fp3'
        g = Gen(['A', 'B', 'C', 'R', 'T', 'U', 'S', 'F'], excl_v=named, nd=3, own_frame=own)
        a = g.a
        g.prologue()
        a.umov_h(g.A, d['rn'], 0)
        if form != 'fp1':
            a.umov_h(g.B, d['rm'], 0)
        if n3:
            a.umov_h(g.C, d['ra'], 0)
        if form == 'fp1':
            core_un(g, nm, g.A)
        elif form == 'fp2':
            core_bin(g, nm, g.A, g.B)
        else:
            core_fma(g, nm, g.A, g.B, g.C)
        a.fmov_s_w(d['rd'], g.R)
        g.epilogue()
        return g.words()
    if form in ('fcmp', 'fccmp'):
        g = Gen(['A', 'B', 'R', 'T', 'U', 'S', 'F'], excl_v=named, nd=2, own_frame=own)
        a = g.a
        g.prologue()
        zero = form == 'fcmp' and d['zero']
        if form == 'fccmp':
            # FCCMP: if cond holds -> FCMP(E); else NZCV := #imm with NO FP exceptions (so no conversions on that path)
            Ltrue = g.lab()
            a.bcond(d['cond'], Ltrue)
            a.mrs_fpsr(g.T)
            a.orr_r(g.S, g.S, g.T, 1)
            a.msr_fpsr(g.S)
            a.movz(g.T, d['nzcv'] << 12, 1, 1)
            a.msr_nzcv(g.T)
            g.restore()
            a.place(Ltrue)
        a.umov_h(g.A, d['rn'], 0)
        if not zero:
            a.umov_h(g.B, d['rm'], 0)
        fz_begin(g, [g.A] if zero else [g.A, g.B], False)
        d0, d1 = g.D[0], g.D[1]
        to_d(g, d0, g.A)
        if not zero:
            to_d(g, d1, g.B)
        # merge FPSR first (flag-less), then the compare as the very last flag-setting op
        a.mrs_fpsr(g.T)
        a.orr_r(g.S, g.S, g.T, 1)
        a.msr_fpsr(g.S)
        Lahp = g.lab()
        a.tbz(g.F, 26, Lahp)
        a.msr_fpcr(g.F)
        a.place(Lahp)
        # restore all but D0/D1 (still live), then compare, then restore D and ret
        g._gsave(False)
        # compare using the (still-live) d regs
        a.fcmp_d(d0, d1, bool(d['e']), bool(zero))
        for i, r in enumerate(g.D):
            a.ldr_q(r, 31, 16 * i)
        if g.own:
            a.add_i(31, 31, g.FS)
        a.ret()
        return g.words()
    if form in ('intcv', 'fixcv'):
        name = d['name']
        sf = d['sf']
        if name == 'fmov_wh':
            a = Asm()
            if d['rd'] != 31:
                a.umov_h(d['rd'], d['rn'], 0)
            a.ret()
            return a.resolve()
        if name == 'fmov_hw':
            a = Asm()
            a.movi_2d_0(d['rd'])
            a.ins_h_w(d['rd'], 0, d['rn'])
            a.ret()
            return a.resolve()
        if name in ('scvtf', 'ucvtf'):
            excl_g = {d['rn']} - {31}
            g = Gen(['R', 'T', 'U', 'S', 'F'], excl_g=excl_g, excl_v=named, nd=2, own_frame=own)
            a = g.a
            g.prologue()
            d0 = g.D[0]
            signed = name == 'scvtf'
            fb = d.get('fbits', 0)
            fo = fb > 0
            if fo:
                fz_begin(g, [], True)
            if sf:
                # 64-bit source: round-to-odd into binary64 (RZ + sticky), then one RMode rounding to half
                a.and_imm(g.T, g.F, 0xfbffffff)
                a.orr_imm(g.T, g.T, 3 << 22, 0)
                a.msr_fpcr(g.T)
                if fb:
                    a.cvtf_fix(signed, d0, d['rn'], fb, 1)
                else:
                    a.cvtf_int(signed, d0, d['rn'], 1)
                a.and_imm(g.T, g.F, 0xfbffffff)
                a.msr_fpcr(g.T)
                Lex = g.lab()
                a.mrs_fpsr(g.T)
                a.tbz(g.T, 4, Lex)
                a.fmov_x_d(g.U, d0)
                a.orr_imm(g.U, g.U, 1, 1)
                a.fmov_d_x(d0, g.U)
                a.and_imm(g.T, g.T, 0xffffffef)
                a.msr_fpsr(g.T)
                a.place(Lex)
            else:
                if fb:
                    a.cvtf_fix(signed, d0, d['rn'], fb, 0)
                else:
                    a.cvtf_int(signed, d0, d['rn'], 0)
            finish(g, d0, fo)
            a.fmov_s_w(d['rd'], g.R)
            g.epilogue()
            return g.words()
        # FCVT** Wd/Xd, Hn [, #fbits]   (Rd==31 is WZR/XZR: result discarded but FPSR flags still raised)
        excl_g = {d['rd']} - {31}
        g = Gen(['A', 'T', 'S', 'F'], excl_g=excl_g, excl_v=named, nd=1, own_frame=own)
        a = g.a
        g.prologue()
        a.umov_h(g.A, d['rn'], 0)
        fz_begin(g, [g.A], False)
        d0 = g.D[0]
        to_d(g, d0, g.A)
        kind = name[3:]
        if form == 'fixcv':
            a.fcvt_fix(kind, d['rd'], d0, d['fbits'], sf)
        else:
            a.fcvt_int(kind, d['rd'], d0, sf)
        g.epilogue()
        return g.words()
    raise NotImplementedError(form)


def _build_bypass(d):
    """no-scratch forms"""
    form = d['form']
    a = Asm()
    if form in ('fmov_imm', 'vfmov'):
        imm = d['imm8']
        A_, B_, C_, D_, E_, F_, G_, H_ = [(imm >> (7 - i)) & 1 for i in range(8)]
        hi = (A_ << 7) | ((1 - B_) << 6) | (B_ << 5) | (B_ << 4) | (((imm >> 4) & 3) << 2) | ((imm >> 2) & 3)
        lo = (imm & 3) << 6
        if form == 'vfmov':
            a.movi_h_lsl8(d['rd'], hi, d['q'])
            if lo:
                a.orr_h_imm(d['rd'], lo, d['q'])
        else:
            a.movi_2s_lsl8(d['rd'], hi)
            if lo:
                a.orr_2s_imm(d['rd'], lo)
            a.fmov_s_s(d['rd'], d['rd'])
    elif form == 'fcsel':
        a.fcsel_s(d['rd'], d['rn'], d['rm'], d['cond'])
        a.dup_h(d['rd'], d['rd'], 0)
    elif form == 'fp1' and d['name'] == 'mov':
        a.dup_h(d['rd'], d['rn'], 0)
    elif form in ('fp1', 's2', 'v2') and d['name'] in ('abs', 'neg'):
        op = a.fabs_s_vec if d['name'] == 'abs' else a.fneg_s_vec
        rd, rn = d['rd'], d['rn']
        if form == 'v2':
            q = d['q']
            op(rd, rn, q)
            a.rev32(rd, rd, q)
            op(rd, rd, q)
            a.rev32(rd, rd, q)
        else:
            a.dup_h(rd, rn, 0)
            a.rev32(rd, rd, 0)
            (a.fabs_s if d['name'] == 'abs' else a.fneg_s)(rd, rd)
            a.rev32(rd, rd, 0)
    else:
        return None
    a.ret()
    return a.resolve()


def stub_words_fp16(w, own_frame=False):
    d = decode(w)
    if d is None or d.get('form') == 'pass':
        return None
    form = d['form']
    nm = d.get('name')
    by = _build_bypass(d) if (form in ('fmov_imm', 'vfmov', 'fcsel') or (form == 'fp1' and nm in ('mov', 'abs', 'neg'))
                              or (form in ('s2', 'v2') and nm in ('abs', 'neg'))) else None
    if by is not None:
        return by
    if form in ('fp2', 'fp3', 'fp1', 'fcmp', 'fccmp', 'intcv', 'fixcv'):
        return _build_fp_scalar(d, own_frame)
    if form in ('s3', 'v3', 's2', 'v2', 'sidx', 'vidx'):
        return _build_elementwise(d, own_frame)
    if form in ('sP', 'vP', 'vA'):
        return _build_pair(d, own_frame)
    return None


if __name__ == '__main__':
    for a_ in sys.argv[1:]:
        w_ = int(a_, 16)
        r_ = stub_words_fp16(w_)
        print('%08x is_fp16=%s -> %s' % (w_, is_fp16(w_), None if r_ is None else ' '.join('%08x' % x for x in r_)))
