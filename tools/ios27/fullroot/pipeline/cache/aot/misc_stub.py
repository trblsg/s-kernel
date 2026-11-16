#!/usr/bin/env python3
"""misc_stub.py - ARMv8.0-only replacements for the mechanical non-ARMv8.0 instruction classes that the
Exynos M3 (ARMv8.0) lacks.  PRIVATE (ios27-rewrite-all / misc, Nov 2026; mini only).

Public API
  rewrite_inplace(w)  -> w'   1:1 same-size replacement (LRCPC LDAPR*, LOR LDLAR/STLLR, LRCPC2 with imm==0), else None
  stub_words_misc(w)  -> [w...] or None
        Out-of-line stub BODY.  Entered by BL from a per-site thunk that has already lowered SP by 144 (past the
        128-byte red zone) and parked x30 -- exactly the contract of lse-rewrite.stub_words(ret=True, redzone=0,
        sp_extra=144).  x30 = return address, the body ends with RET.  It saves/restores every scratch X/V register
        it uses in its own frame below SP (<= 144 bytes), and preserves every register, NZCV and FPSR except the
        instruction's documented destination(s) and the status bits the instruction itself defines.
        None = class not handled here (f16 lanes -> fp16 module, x30 operands, ...).
  classify(w) -> (class, name) or None   decoder used by the harness
  plan(w)     -> ('inplace', word) | ('stub', [words]) | None   one-call front end

Classes: LRCPC2 LDAPUR/STLUR, DotProd SDOT/UDOT, RDM SQRDMLAH/SQRDMLSH, FCMA FCADD/FCMLA (f32/f64),
FHM FMLAL{,2}/FMLSL{,2}, I8MM USDOT/SUDOT/SMMLA/UMMLA/USMMLA, BF16 BFDOT/BFMMLA/BFMLALB/T/BFCVT/BFCVTN{,2},
SHA3 EOR3/BCAX/RAX1/XAR (+SHA512H/H2/SU0/SU1), JSCVT FJCVTZS.
"""

RET = 0xD65F03C0
FRAME_MAX = 144
THUNK_SP = 144            # SP the caller's thunk lowered before BL (matters for SP-relative operands)


# ---------------------------------------------------------------------------------------------------
# encoders (AdvSIMD + a few integer forms)
def v3(Q, U, size, opc, d, n, m):            # AdvSIMD three same
    return Q << 30 | U << 29 | 0x0E << 24 | size << 22 | 1 << 21 | m << 16 | opc << 11 | 1 << 10 | n << 5 | d


def v2(Q, U, size, opc, d, n):               # AdvSIMD two-register misc
    return Q << 30 | U << 29 | 0x0E << 24 | size << 22 | 0b10000 << 17 | opc << 12 | 0b10 << 10 | n << 5 | d


def v3d(Q, U, size, opc, d, n, m):           # AdvSIMD three different
    return Q << 30 | U << 29 | 0x0E << 24 | size << 22 | 1 << 21 | m << 16 | opc << 12 | n << 5 | d


def vacross(Q, U, size, opc, d, n):
    return Q << 30 | U << 29 | 0x0E << 24 | size << 22 | 0b11000 << 17 | opc << 12 | 0b10 << 10 | n << 5 | d


def vperm(Q, size, opc, d, n, m):
    return Q << 30 | 0x0E << 24 | size << 22 | m << 16 | opc << 12 | 0b10 << 10 | n << 5 | d


def vshift(Q, U, immhb, opc, d, n):          # AdvSIMD shift by immediate (immhb = immh:immb, 7 bits)
    return Q << 30 | U << 29 | 0x0F << 24 | immhb << 16 | opc << 11 | 1 << 10 | n << 5 | d


def vidx(Q, U, size, L, M, Rm, opc, H, n, d):   # AdvSIMD vector x indexed element
    return Q << 30 | U << 29 | 0x0F << 24 | size << 22 | L << 21 | M << 20 | Rm << 16 | opc << 12 | H << 11 | n << 5 | d


def eor_(d, n, m, Q=1):  return v3(Q, 1, 0, 0b00011, d, n, m)
def bic_(d, n, m, Q=1):  return v3(Q, 0, 1, 0b00011, d, n, m)
def orr_(d, n, m, Q=1):  return v3(Q, 0, 2, 0b00011, d, n, m)
def mov_(d, n, Q=1):     return v3(Q, 0, 2, 0b00011, d, n, n)
def add_(sz, d, n, m, Q=1): return v3(Q, 0, sz, 0b10000, d, n, m)
def sub_(sz, d, n, m, Q=1): return v3(Q, 1, sz, 0b10000, d, n, m)
def mul_(sz, d, n, m, Q=1): return v3(Q, 0, sz, 0b10011, d, n, m)
def addp_(sz, d, n, m, Q=1): return v3(Q, 0, sz, 0b10111, d, n, m)
def fadd_(sz, d, n, m, Q=1): return v3(Q, 0, sz, 0b11010, d, n, m)          # sz 0 = .4s/.2s  1 = .2d
def fmla_(sz, d, n, m, Q=1): return v3(Q, 0, sz, 0b11001, d, n, m)
def fmls_(sz, d, n, m, Q=1): return v3(Q, 0, 2 | sz, 0b11001, d, n, m)
def fneg_(sz, d, n, Q=1):   return v2(Q, 1, 2 | sz, 0b01111, d, n)
def rev64_(size, d, n, Q=1): return v2(Q, 0, size, 0b00000, d, n)
def saddlp_(size, d, n, Q=1): return v2(Q, 0, size, 0b00010, d, n)
def uaddlp_(size, d, n, Q=1): return v2(Q, 1, size, 0b00010, d, n)
def fcvtl_(d, n, Q=0):      return v2(Q, 0, 0, 0b10111, d, n)               # .4h -> .4s (Q=1: FCVTL2 .8h)
def sqxtn_(size, d, n, Q=0): return v2(Q, 0, size, 0b10100, d, n)           # size = destination element size
def trn1_(size, d, n, m, Q=1): return vperm(Q, size, 0b010, d, n, m)
def trn2_(size, d, n, m, Q=1): return vperm(Q, size, 0b110, d, n, m)
def ext_(d, n, m, imm, Q=1): return Q << 30 | 0x2E << 24 | m << 16 | imm << 11 | n << 5 | d
def saddlv_(size, d, n, Q=1): return vacross(Q, 0, size, 0b00011, d, n)
def uaddlv_(size, d, n, Q=1): return vacross(Q, 1, size, 0b00011, d, n)
def dup_el(Q, imm5, d, n):  return 0x0E000400 | Q << 30 | imm5 << 16 | n << 5 | d
def ins_el(imm5, imm4, d, n): return 0x6E000400 | imm5 << 16 | imm4 << 11 | n << 5 | d
def ins_gp_d(idx, d, n):    return 0x4E001C00 | (idx << 4 | 8) << 16 | n << 5 | d
def smull_(Q, U, size, d, n, m): return v3d(Q, U, size, 0b1100, d, n, m)    # size = source element size
def xtl_(Q, U, esize_bits, d, n):    # SXTL/UXTL (SSHLL/USHLL #0): esize 8/16/32
    return vshift(Q, U, esize_bits, 0b10100, d, n)
def shl_(Q, imm, esize_bits, d, n):  return vshift(Q, 0, esize_bits + imm, 0b01010, d, n)
def ushr_(Q, imm, esize_bits, d, n): return vshift(Q, 1, 2 * esize_bits - imm, 0b00000, d, n)
def usra_(Q, imm, esize_bits, d, n): return vshift(Q, 1, 2 * esize_bits - imm, 0b00010, d, n)
def srshr_(Q, imm, esize_bits, d, n): return vshift(Q, 0, 2 * esize_bits - imm, 0b00100, d, n)
def movi_2d(Q, imm8, d):
    abc, defgh = imm8 >> 5, imm8 & 31
    return 0x2F00E400 | Q << 30 | abc << 16 | defgh << 5 | d

def neg_(sz, d, n, Q=1):   return v2(Q, 1, sz, 0b01011, d, n)
def cmeq0_(sz, d, n, Q=1): return v2(Q, 0, sz, 0b01001, d, n)
def cmeq_(sz, d, n, m, Q=1): return v3(Q, 1, sz, 0b10001, d, n, m)
def and_(d, n, m, Q=1):    return v3(Q, 0, 0, 0b00011, d, n, m)
def movi32(Q, imm8, shift, d):   # MOVI Vd.4S, #imm8, LSL #shift (0/8/16/24)
    return 0x0F000400 | Q << 30 | (imm8 >> 5) << 16 | (shift // 8 * 2) << 12 | (imm8 & 31) << 5 | d
def mvni32(Q, imm8, shift, d):  return 0x2F000400 | Q << 30 | (imm8 >> 5) << 16 | (shift // 8 * 2) << 12 | (imm8 & 31) << 5 | d
def mrs_fpcr(t): return 0xD53B4400 | t
def sbfx_w(d, n, lsb, width): return 0x13000000 | lsb << 16 | (lsb + width - 1) << 10 | n << 5 | d
def dup_w(d, n): return 0x4E040C00 | n << 5 | d
def umaxv_(d, n): return vacross(1, 1, 2, 0b01010, d, n)
def fmov_ws(d, n): return 0x1E260000 | n << 5 | d
def and_w_0x80(d, n): return 0x12190000 | n << 5 | d                    # AND Wd, Wn, #0x80
def fmov_ss(d, n):         return 0x1E204000 | n << 5 | d
def ushr_d(d, n, sh):      return 0x7F000400 | (128 - sh) << 16 | n << 5 | d      # USHR Dd, Dn, #sh (scalar)

# integer
def and_x(d, n, m):   return 0x8A000000 | m << 16 | n << 5 | d
def bic_x(d, n, m):   return 0x8A200000 | m << 16 | n << 5 | d
def eor1_x(d, n):     return 0xD2400000 | n << 5 | d                     # EOR Xd, Xn, #1
def lsr_xi(d, n, sh): return 0xD340FC00 | sh << 16 | n << 5 | d
def sub_sp(n):   return 0xD10003FF | n << 10
def add_sp(n):   return 0x910003FF | n << 10
def stp_q(a, b, off): return 0xAD000000 | (off // 16 & 0x7F) << 15 | b << 10 | 31 << 5 | a
def ldp_q(a, b, off): return 0xAD400000 | (off // 16 & 0x7F) << 15 | b << 10 | 31 << 5 | a
def str_q(a, off):    return 0x3D800000 | (off // 16) << 10 | 31 << 5 | a
def ldr_q(a, off):    return 0x3DC00000 | (off // 16) << 10 | 31 << 5 | a
def stp_x(a, b, off): return 0xA9000000 | (off // 8 & 0x7F) << 15 | b << 10 | 31 << 5 | a
def ldp_x(a, b, off): return 0xA9400000 | (off // 8 & 0x7F) << 15 | b << 10 | 31 << 5 | a
def str_x(a, off):    return 0xF9000000 | (off // 8) << 10 | 31 << 5 | a
def ldr_x(a, off):    return 0xF9400000 | (off // 8) << 10 | 31 << 5 | a
def fmov_xd(d, n):    return 0x9E660000 | n << 5 | d        # FMOV Xd, Dn
def fcvtzs_xd(d, n):  return 0x9E780000 | n << 5 | d
def frintz_d(d, n):   return 0x1E65C000 | n << 5 | d
def fcmp_d(n, m):     return 0x1E602000 | m << 16 | n << 5
def cset(d, cond):    return 0x1A9F07E0 | (cond ^ 1) << 12 | d        # CSET Wd, cond
def cmp_xi(n, imm):   return 0xF100001F | imm << 10 | n << 5
def cmp_x_sxtw(n, m): return 0xEB20C01F | m << 16 | n << 5
def csel_x(d, n, m, cond): return 0x9A800000 | m << 16 | cond << 12 | n << 5 | d
def csel_w(d, n, m, cond): return 0x1A800000 | m << 16 | cond << 12 | n << 5 | d
def ubfx_x(d, n, lsb, width): return 0xD3400000 | lsb << 16 | (lsb + width - 1) << 10 | n << 5 | d
def movz_x48(d, imm16): return 0xD2E00000 | imm16 << 5 | d
def orr_x(d, n, m):   return 0xAA000000 | m << 16 | n << 5 | d
def orr_w(d, n, m):   return 0x2A000000 | m << 16 | n << 5 | d
def sub_xi(d, n, imm): return 0xD1000000 | imm << 10 | n << 5 | d
def lslv_x(d, n, m): return 0x9AC02000 | m << 16 | n << 5 | d
def neg_x(d, m):      return 0xCB0003E0 | m << 16 | d
def lsl_xi(d, n, s): return 0xD3400000 | (-s & 63) << 16 | (63 - s) << 10 | n << 5 | d
def mrs_fpsr(t): return 0xD53B4420 | t
def msr_fpsr(t): return 0xD51B4420 | t
def msr_nzcv(t): return 0xD51B4200 | t
EQ, NE, HS, LO, LT, VS = 0, 1, 2, 3, 11, 6
DMB_ISH, DMB_ISHLD = 0xD5033BBF, 0xD50339BF


# ---------------------------------------------------------------------------------------------------
# frame handling
def _wrap(body, vs=(), xs=()):
    """prologue (SUB SP + save scratch) + body + epilogue + RET."""
    vs, xs = list(vs), list(xs)
    size = (16 * len(vs) + 8 * len(xs) + 15) & ~15
    assert size <= FRAME_MAX, size
    pro, epi = [], []
    if size:
        pro.append(sub_sp(size))
    off = 0
    i = 0
    while i < len(vs):
        if i + 1 < len(vs):
            pro.append(stp_q(vs[i], vs[i + 1], off)); epi.append(ldp_q(vs[i], vs[i + 1], off)); i += 2; off += 32
        else:
            pro.append(str_q(vs[i], off)); epi.append(ldr_q(vs[i], off)); i += 1; off += 16
    i = 0
    while i < len(xs):
        if i + 1 < len(xs):
            pro.append(stp_x(xs[i], xs[i + 1], off)); epi.append(ldp_x(xs[i], xs[i + 1], off)); i += 2; off += 16
        else:
            pro.append(str_x(xs[i], off)); epi.append(ldr_x(xs[i], off)); i += 1; off += 8
    if size:
        epi.append(add_sp(size))
    return pro + list(body) + epi + [RET]


def _vscr(avoid, n):
    out = [r for r in list(range(16, 32)) + list(range(0, 16)) if r not in avoid][:n]
    assert len(out) == n
    return out


def _xscr(avoid, n):
    out = [r for r in range(9, 18) if r not in avoid][:n]
    assert len(out) == n
    return out


# ---------------------------------------------------------------------------------------------------
# decoder
def classify(w):
    """-> (class, mnemonic) for the instructions this module handles, else None."""
    if (w & 0x3F200C00) == 0x19000000 and not ((w >> 30) == 2 and (w >> 22) & 3 == 3) and \
            not ((w >> 30) == 3 and (w >> 23) & 1):
        sz, opc = w >> 30, (w >> 22) & 3
        if opc == 0:
            return 'LRCPC2', 'stlur'
        if opc == 1:
            return 'LRCPC2', 'ldapur'
        return 'LRCPC2', 'ldapurs'
    if (w & 0x3FE0FC00) == 0x38A0C000:          # Rs field is SBO; llvm decodes it for any Rs
        return 'LRCPC', 'ldapr'
    if (w & 0x3FE08000) == 0x08C00000:          # LDLAR (Rs / Rt2 are SBO)
        return 'LOR', 'ldlar'
    if (w & 0x3FE08000) == 0x08800000:
        return 'LOR', 'stllr'
    if (w & 0x9FE0FC00) == 0x0E809400:
        return 'DotProd', 'udot' if w >> 29 & 1 else 'sdot'
    if (w & 0x9FC0F400) == 0x0F80E000:
        return 'DotProd', 'udot_el' if w >> 29 & 1 else 'sdot_el'
    sz = (w >> 22) & 3
    if sz in (1, 2):
        if (w & 0xBF20F400) == 0x2E008400 or (w & 0xFF20F400) == 0x7E008400:
            return 'RDM', 'sqrdmlsh' if w >> 11 & 1 else 'sqrdmlah'
        if (w & 0xBF00D400) == 0x2F00D000 or (w & 0xFF00D400) == 0x7F00D000:
            return 'RDM', 'sqrdmlsh_el' if w >> 13 & 1 else 'sqrdmlah_el'
    Qb = w >> 30 & 1
    if (w & 0xBF20EC00) == 0x2E00E400 and sz != 0 and (sz != 3 or Qb):
        return 'FCMA', 'fcadd'
    if (w & 0xBF20E400) == 0x2E00C400 and sz != 0 and (sz != 3 or Qb):
        return 'FCMA', 'fcmla'
    if (w & 0xBF009400) == 0x2F001000 and ((sz == 2 and Qb and not (w >> 21 & 1)) or (sz == 1 and (Qb or not (w >> 11 & 1)))):
        return 'FCMA', 'fcmla_el'
    if (w & 0xBF60FC00) == 0x0E20EC00:
        return 'FHM', 'fmlsl' if w >> 23 & 1 else 'fmlal'
    if (w & 0xBF60FC00) == 0x2E20CC00:
        return 'FHM', 'fmlsl2' if w >> 23 & 1 else 'fmlal2'
    if (w & 0x9FC03400) == 0x0F800000 and ((w >> 29) & 1) == ((w >> 15) & 1):
        return 'FHM', 'fhm_el'
    if (w & 0xBFE0FC00) == 0x0E809C00:
        return 'I8MM', 'usdot'
    if (w & 0xBFC0F400) == 0x0F80F000:
        return 'I8MM', 'usdot_el'
    if (w & 0xBFC0F400) == 0x0F00F000:
        return 'I8MM', 'sudot_el'
    if (w & 0xFFE0FC00) == 0x4E80A400: return 'I8MM', 'smmla'
    if (w & 0xFFE0FC00) == 0x6E80A400: return 'I8MM', 'ummla'
    if (w & 0xFFE0FC00) == 0x4E80AC00: return 'I8MM', 'usmmla'
    if (w & 0xBFE0FC00) == 0x2E40FC00: return 'BF16', 'bfdot'
    if (w & 0xBFC0F400) == 0x0F40F000: return 'BF16', 'bfdot_el'
    if (w & 0xFFE0FC00) == 0x6E40EC00: return 'BF16', 'bfmmla'
    if (w & 0xBFE0FC00) == 0x2EC0FC00: return 'BF16', 'bfmlal'
    if (w & 0xBFC0F400) == 0x0FC0F000: return 'BF16', 'bfmlal_el'
    if (w & 0xFFFFFC00) == 0x1E634000: return 'BF16', 'bfcvt'
    if (w & 0xBFFFFC00) == 0x0EA16800: return 'BF16', 'bfcvtn'
    if (w & 0xFFE08000) == 0xCE000000: return 'SHA3', 'eor3'
    if (w & 0xFFE08000) == 0xCE200000: return 'SHA3', 'bcax'
    if (w & 0xFFE0FC00) == 0xCE608C00: return 'SHA3', 'rax1'
    if (w & 0xFFE00000) == 0xCE800000: return 'SHA3', 'xar'
    if (w & 0xFFE0FC00) == 0xCE608000: return 'SHA3', 'sha512h'
    if (w & 0xFFE0FC00) == 0xCE608400: return 'SHA3', 'sha512h2'
    if (w & 0xFFFFFC00) == 0xCEC08000: return 'SHA3', 'sha512su0'
    if (w & 0xFFE0FC00) == 0xCE608800: return 'SHA3', 'sha512su1'
    if (w & 0xFFFFFC00) == 0x1E7E0000:
        return 'JSCVT', 'fjcvtzs'
    return None


# ---------------------------------------------------------------------------------------------------
# in-place 1:1 rewrites
def rewrite_inplace(w):
    c = classify(w)
    if not c:
        return None
    k = c[1]
    if k in ('ldapr', 'ldlar'):                       # LDAPR/LDLAR{,B,H} -> LDAR{,B,H}
        return (w & 0xC00003FF) | 0x08DFFC00
    if k == 'stllr':
        return (w & 0xC00003FF) | 0x089FFC00          # STLLR -> STLR
    if c[0] == 'LRCPC2' and ((w >> 12) & 0x1FF) == 0:  # imm9 == 0, no sign extension
        sz, opc = w >> 30, (w >> 22) & 3
        if k == 'stlur':
            return (w & 0xC00003FF) | 0x089FFC00
        if k == 'ldapur':
            return (w & 0xC00003FF) | 0x08DFFC00
    return None


# ---------------------------------------------------------------------------------------------------
def _lrcpc2(w, thunk_sp=THUNK_SP):
    sz, opc = w >> 30, (w >> 22) & 3
    imm9 = (w >> 12) & 0x1FF
    imm = imm9 - 512 if imm9 & 0x100 else imm9
    rn, rt = (w >> 5) & 31, w & 31
    if rt == 30 or rn == 30:
        return None
    body, xs = [], []
    if rn == 31:
        t = _xscr({rt}, 1)[0]
        xs = [t]
        ofs = thunk_sp + 16 + imm                 # original SP + imm (thunk lowered SP by thunk_sp, our frame is 16)
        body.append((0x91000000 | ofs << 10 | 31 << 5 | t) if ofs >= 0 else (0xD1000000 | (-ofs) << 10 | 31 << 5 | t))
        acc = 0x38000000 | sz << 30 | opc << 22 | 0 << 12 | t << 5 | rt
    else:
        acc = 0x38000000 | sz << 30 | opc << 22 | imm9 << 12 | rn << 5 | rt       # LDUR*/STUR* same size/opc
    body += [DMB_ISH, acc] if opc == 0 else [acc, DMB_ISHLD]
    return _wrap(body, (), xs)


# ---------------------------------------------------------------------------------------------------
def _dotprod(w, k):
    Q, U = w >> 30 & 1, w >> 29 & 1
    rd, rn = w & 31, (w >> 5) & 31
    el = k.endswith('_el')
    if el:
        idx = (w >> 11 & 1) << 1 | (w >> 21 & 1)
        rm = (w >> 20 & 1) << 4 | (w >> 16 & 15)
    else:
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 4)
    t1, t2, t3, tm = vs
    body = []
    m = rm
    if el:
        body.append(dup_el(1, idx << 3 | 4, tm, rm)); m = tm
    mull = (lambda q, d, n, mm: smull_(q, U, 0, d, n, mm))
    addlp = uaddlp_ if U else saddlp_
    body.append(mull(0, t1, rn, m))
    if Q:
        body += [mull(1, t2, rn, m), addlp(1, t1, t1), addlp(1, t2, t2), addp_(2, t1, t1, t2), add_(2, rd, rd, t1)]
    else:
        body += [addlp(1, t1, t1), addp_(2, t1, t1, t1), add_(2, rd, rd, t1, 0)]
    return _wrap(body, vs)


def _rdm(w, k):
    scalar = (w >> 28 & 3) == 3            # 01 U 11110 / 01 1 11111
    el = k.endswith('_el')
    sub = (w >> 13 & 1) if el else (w >> 11 & 1)
    Q = w >> 30 & 1
    size = w >> 22 & 3
    rd, rn = w & 31, (w >> 5) & 31
    if el:
        if size == 1:
            idx = (w >> 11 & 1) << 2 | (w >> 21 & 1) << 1 | (w >> 20 & 1)
            rm = w >> 16 & 15
        else:
            idx = (w >> 11 & 1) << 1 | (w >> 21 & 1)
            rm = (w >> 20 & 1) << 4 | (w >> 16 & 15)
    else:
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 3)
    t1, t2, t3 = vs
    hw = size == 1
    sh, wide = (15, 15) if hw else (31, 31)
    # widening multiply: .4h->.4s (size 1) / .2s->.2d (size 2): smull size field = source esize code
    def mul(q, d):
        if el:
            # SMULL{2} by element: 0 Q 0 01111 size L M Rm 1010 H 0 Rn Rd
            if hw:
                return vidx(q, 0, 1, idx >> 1 & 1, idx & 1, rm, 0b1010, idx >> 2 & 1, rn, d)
            return vidx(q, 0, 2, idx & 1, rm >> 4 & 1, rm & 15, 0b1010, idx >> 1 & 1, rn, d)
        return smull_(q, 0, size, d, rn, rm)
    # esize bits of source: 16 or 32 ; widened lanes 32 or 64
    esrc = 16 if hw else 32
    shr = 15 if hw else 31
    wsz = 2 if hw else 3          # (not used for ops with explicit imm)
    body = []
    lowfull = not scalar
    body.append(mul(0, t1))
    if sub:
        body.append(neg_(wsz, t1, t1, 1))
    body.append(srshr_(1, shr, esrc * 2, t1, t1))
    body.append(xtl_(0, 0, esrc, t3, rd))
    body.append(add_(wsz, t1, t3, t1, 1))
    if scalar:
        # SQXTN Hd, Sn / Sd, Dn  (scalar): 01 0 11110 size 1 0000 10100 10 Rn Rd
        body.append(0x5E214800 | size << 22 | t1 << 5 | rd)
    else:
        if Q:
            body.append(mul(1, t2))
            if sub:
                body.append(neg_(wsz, t2, t2, 1))
            body.append(srshr_(1, shr, esrc * 2, t2, t2))
            body.append(xtl_(1, 0, esrc, t3, rd))
            body.append(add_(wsz, t2, t3, t2, 1))
            body.append(sqxtn_(size, rd, t1, 0))
            body.append(sqxtn_(size, rd, t2, 1))
        else:
            body.append(sqxtn_(size, rd, t1, 0))
    return _wrap(body, vs)


def _fcma(w, k):
    Q = w >> 30 & 1
    size = w >> 22 & 3
    if size == 1:
        return None                                # f16 lanes -> fp16 module
    sz = 1 if size == 3 else 0
    rd, rn = w & 31, (w >> 5) & 31

    def neg_lanes(tgt, tneg, odd):                 # tgt.lane[i] <- tneg.lane[i] for even (odd=0) / odd (odd=1) lanes
        if sz:
            lanes = [odd]
        else:
            lanes = [odd, odd + 2] if Q else [odd]
        out = []
        for i in lanes:
            out.append(ins_el(i << 4 | 8, i << 3, tgt, tneg) if sz else ins_el(i << 3 | 4, i << 2, tgt, tneg))
        return out

    def swap_pairs(dst, src):
        return ext_(dst, src, src, 8) if sz else rev64_(2, dst, src, Q)

    if k == 'fcadd':
        rm = w >> 16 & 31
        rot = (w >> 12) & 1                        # 0 = 90, 1 = 270
        vs = _vscr({rd, rn, rm}, 2)
        t1, t2 = vs
        body = [swap_pairs(t1, rm), fneg_(sz, t2, t1, Q)]
        body += neg_lanes(t1, t2, rot)            # rot 90: -Vm[odd] -> even lane ; rot 270: -Vm[even] -> odd lane
        body.append(fadd_(sz, rd, rn, t1, Q))
        return _wrap(body, vs)
    el = k == 'fcmla_el'
    if el:
        rot = (w >> 13) & 3
        H = w >> 11 & 1
        rm = (w >> 20 & 1) << 4 | (w >> 16 & 15)
        if size != 2 or not Q:
            return None
    else:
        rot = (w >> 11) & 3
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 4)
    ta, tb, tc, td = vs
    body = []
    b = rm
    if el:
        body.append(dup_el(1, H << 4 | 8, tc, rm)); b = tc          # replicate the selected complex pair
    body.append((trn1_ if rot in (0, 2) else trn2_)(3 if sz else 2, ta, rn, rn, Q))   # a.re (or a.im) pairwise
    if rot == 0:
        body.append(fmla_(sz, rd, ta, b, Q))
    elif rot == 2:
        body += [fneg_(sz, tb, b, Q), fmla_(sz, rd, ta, tb, Q)]
    else:
        # rot 90: [-b1, b0] (even lanes negated) ; rot 270: [b1, -b0] (odd lanes negated)
        body += [swap_pairs(tb, b), fneg_(sz, td, tb, Q)]
        body += neg_lanes(tb, td, 0 if rot == 1 else 1)
        body.append(fmla_(sz, rd, ta, tb, Q))
    return _wrap(body, vs)




def _fhm(w, k):
    Q = w >> 30 & 1
    rd, rn = w & 31, (w >> 5) & 31
    el = k.endswith('_el')
    sub = (w >> 14 & 1) if el else (w >> 23 & 1)
    part = w >> 29 & 1
    if el:
        idx = (w >> 11 & 1) << 2 | (w >> 21 & 1) << 1 | (w >> 20 & 1)
        rm = w >> 16 & 15
    else:
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 8)
    k1, k2, k3, c1, c2, t, u, h = vs
    x9, = _xscr(set(), 1)
    body = [movi32(1, 0x7e, 8, k1), movi32(1, 0x7c, 8, k2), movi32(1, 0x40, 16, k3)]

    def participating(src):
        """-> (list of insns, reg) with the 4 participating halves in the low 64 bits (upper 64 bits zero or unused)."""
        if Q and not part:
            return [], src                              # halves 0..3 are the low 64 bits already
        if Q and part:
            return [ext_(h, src, src, 8)], h            # halves 4..7
        if not part:
            return [fmov_ss(h, src)], h                 # halves 0,1 only (others zero: no spurious flags)
        return [ushr_d(h, src, 32)], h                  # halves 2,3 only

    def widen(dst, src, dup_idx=None):
        out = []
        if dup_idx is not None:
            out.append(dup_el(0, dup_idx << 2 | 2, h, src)); hh = h      # .4h of one element (no extra lanes)
        else:
            ins, hh = participating(src); out += ins
        out += [fcvtl_(dst, hh, 0),                    # exact half -> single (no flags for quiet inputs)
                xtl_(0, 1, 16, t, hh),                 # raw half bits in 32-bit lanes
                and_(u, t, k1), cmeq_(2, u, u, k2),    # exp == 31 and quiet bit clear
                shl_(1, 23, 32, t, t), cmeq0_(2, t, t),  # t = (mantissa low 9 bits == 0)
                bic_(u, u, t), and_(u, u, k3),         # u = bit 22 in lanes holding a half SNaN
                bic_(dst, dst, u),                     # un-quiet it (IOC + SNaN priority exactly as native)
                shl_(1, 9, 32, t, dst), cmeq0_(2, t, t), and_(t, t, u), ushr_(1, 1, 32, t, t), orr_(dst, dst, t),  # DN: payload 0 -> keep a NaN
                # FPCR.FZ16: half denormal inputs are flushed to signed zero
                xtl_(0, 1, 16, t, hh), and_(u, t, k2), cmeq0_(2, u, u), shl_(1, 22, 32, t, t), cmeq0_(2, t, t), bic_(u, u, t),
                mrs_fpcr(x9), sbfx_w(x9, x9, 19, 1), dup_w(t, x9), and_(u, u, t),
                mvni32(1, 0x80, 24, t), and_(t, u, t), bic_(dst, dst, t)]      # (native raises no IDC for the FZ16 flush)
        return out
    body += widen(c1, rn)
    body += widen(c2, rm, idx) if el else widen(c2, rm)
    body.append((fmls_ if sub else fmla_)(0, rd, c1, c2, Q))
    return _wrap(body, vs, (x9,))


def _i8mm(w, k):
    rd, rn = w & 31, (w >> 5) & 31
    Q = w >> 30 & 1
    if k in ('smmla', 'ummla', 'usmmla'):
        rm = w >> 16 & 31
        n_signed = k == 'smmla' or k == 'smmla'
        nU = 1 if k in ('ummla', 'usmmla') else 0        # Vn unsigned?
        mU = 1 if k == 'ummla' else 0                    # Vm unsigned?
        vs = _vscr({rd, rn, rm}, 6)
        t1, t2, t3, t4, t5, t6 = vs
        body = [xtl_(0, nU, 8, t1, rn), xtl_(1, nU, 8, t2, rn), xtl_(0, mU, 8, t3, rm), xtl_(1, mU, 8, t4, rm)]
        lv = uaddlv_ if k == 'ummla' else saddlv_
        for i, (a, b) in enumerate(((t1, t3), (t1, t4), (t2, t3), (t2, t4))):
            body += [mul_(1, t6, a, b), lv(1, t6, t6), ins_el(i << 3 | 4, 0, t5, t6)]
        body.append(add_(2, rd, rd, t5))
        return _wrap(body, vs)
    el = k.endswith('_el')
    if el:
        idx = (w >> 11 & 1) << 1 | (w >> 21 & 1)
        rm = (w >> 20 & 1) << 4 | (w >> 16 & 15)
    else:
        rm = w >> 16 & 31
    nU, mU = (0, 1) if k == 'sudot_el' else (1, 0)       # USDOT: n unsigned, m signed
    vs = _vscr({rd, rn, rm}, 4)
    t1, t2, t3, tm = vs
    m = rm
    body = []
    if el:
        body.append(dup_el(1, idx << 3 | 4, tm, rm)); m = tm
    body += [xtl_(0, nU, 8, t1, rn), xtl_(0, mU, 8, t2, m), mul_(1, t1, t1, t2)]
    if Q:
        body += [xtl_(1, nU, 8, t2, rn), xtl_(1, mU, 8, t3, m), mul_(1, t2, t2, t3),
                 saddlp_(1, t1, t1), saddlp_(1, t2, t2), addp_(2, t1, t1, t2), add_(2, rd, rd, t1)]
    else:
        body += [saddlp_(1, t1, t1), addp_(2, t1, t1, t1), add_(2, rd, rd, t1, 0)]
    return _wrap(body, vs)


def _sha3(w, k):
    rd, rn = w & 31, (w >> 5) & 31
    rm = w >> 16 & 31
    ra = w >> 10 & 31
    if k == 'eor3':
        t, = _vscr({rd, rn, rm, ra}, 1)
        return _wrap([eor_(t, rn, rm), eor_(rd, t, ra)], (t,))
    if k == 'bcax':
        t, = _vscr({rd, rn, rm, ra}, 1)
        return _wrap([bic_(t, rm, ra), eor_(rd, rn, t)], (t,))
    if k == 'rax1':
        t, = _vscr({rd, rn, rm}, 1)
        return _wrap([shl_(1, 1, 64, t, rm), usra_(1, 63, 64, t, rm), eor_(rd, rn, t)], (t,))
    if k == 'xar':
        imm6 = w >> 10 & 63
        t, u = _vscr({rd, rn, rm}, 2)
        if imm6 == 0:
            return _wrap([eor_(rd, rn, rm)], ())
        return _wrap([eor_(t, rn, rm), shl_(1, 64 - imm6, 64, u, t), usra_(1, imm6, 64, u, t), mov_(rd, u)], (t, u))
    if k.startswith('sha512'):
        return _sha512(w, k)
    return None


# ---------------------------------------------------------------------------------------------------
# SHA-512 (FEAT_SHA512: SHA512H/H2/SU0/SU1), scalar 64-bit code on the two halves of each operand
def umov_d(d, n, i): return 0x4E003C00 | (i << 4 | 8) << 16 | n << 5 | d
def x_ror(d, n, r):  return 0x93C00000 | n << 16 | r << 10 | n << 5 | d              # EXTR Xd, Xn, Xn, #r
def x_eor(d, n, m):  return 0xCA000000 | m << 16 | n << 5 | d
def x_add(d, n, m):  return 0x8B000000 | m << 16 | n << 5 | d
def fmov_dx(d, n):   return 0x9E670000 | n << 5 | d


def _sha512(w, k):
    rd, rn, rm = w & 31, (w >> 5) & 31, w >> 16 & 31
    xs = _xscr(set(), 9)
    a, b, c, t, u, y0, y1, x0, x1 = xs[:9]
    # NB: x regs here: a,b,c,t scratch; others hold operand halves
    def ror3(dst, src, r1, r2, r3, tmp):
        return [x_ror(dst, src, r1), x_ror(tmp, src, r2), x_eor(dst, dst, tmp), x_ror(tmp, src, r3), x_eor(dst, dst, tmp)]
    body = []
    if k == 'sha512su0':                    # Vd = SU0(Vd, Vn)
        # X = Vd ; Y = Vn :  lo = X.lo + s0(X.hi) ; hi = X.hi + s0(Y.lo)
        body += [umov_d(x0, rd, 0), umov_d(x1, rd, 1), umov_d(y0, rn, 0)]
        for src, acc, dst in ((x1, x0, a), (y0, x1, b)):
            body += [x_ror(t, src, 1), x_ror(u, src, 8), x_eor(t, t, u), lsr_xi(u, src, 7), x_eor(t, t, u), x_add(dst, acc, t)]
        body += [fmov_dx(rd, a), ins_gp_d(1, rd, b)]
    elif k == 'sha512su1':                  # Vd = SU1(Vd, Vn, Vm) :  lane = X.lane + s1(Y.lane) + W.lane
        body += [umov_d(x0, rd, 0), umov_d(x1, rd, 1), umov_d(y0, rn, 0), umov_d(y1, rn, 1), umov_d(a, rm, 0), umov_d(b, rm, 1)]
        for lane_y, lane_x, lane_w, dst in ((y1, x1, b, c), (y0, x0, a, t)):
            body += [x_ror(u, lane_y, 19)]
            body += [x_ror(dst, lane_y, 61), x_eor(u, u, dst), lsr_xi(dst, lane_y, 6), x_eor(u, u, dst), x_add(u, u, lane_x), x_add(dst, u, lane_w)]
        body += [fmov_dx(rd, t), ins_gp_d(1, rd, c)]
    elif k == 'sha512h':                    # Qd(W), Qn(X), Vm(Y)
        body += [umov_d(a, rd, 0), umov_d(b, rd, 1), umov_d(x0, rn, 0), umov_d(x1, rn, 1), umov_d(y0, rm, 0), umov_d(y1, rm, 1)]
        # a = W.lo, b = W.hi
        body += ror3(c, y1, 14, 18, 41, t)                                                   # c = MSigma1(Y.hi)
        body += [0x8A000000 | x0 << 16 | y1 << 5 | t, 0x8A200000 | y1 << 16 | x1 << 5 | u, x_eor(t, t, u),   # t = (Y.hi & X.lo) ^ (X.hi & ~Y.hi)
                 x_add(t, t, c), x_add(t, t, b)]                                              # t = new hi
        body += [x_add(u, t, y0)]                                                             # u = tmp
        body += ror3(c, u, 14, 18, 41, b)
        body += [0x8A000000 | y1 << 16 | u << 5 | b, 0x8A200000 | u << 16 | x0 << 5 | x1, x_eor(b, b, x1),
                 x_add(b, b, c), x_add(b, b, a)]                                              # b = new lo
        body += [fmov_dx(rd, b), ins_gp_d(1, rd, t)]
    elif k == 'sha512h2':
        body += [umov_d(a, rd, 0), umov_d(b, rd, 1), umov_d(x0, rn, 0), umov_d(y0, rm, 0), umov_d(y1, rm, 1)]
        body += ror3(c, y0, 28, 34, 39, t)                                                   # c = NSigma0(Y.lo)
        and_ = lambda d_, n_, m_: 0x8A000000 | m_ << 16 | n_ << 5 | d_
        body += [and_(t, x0, y1), and_(u, x0, y0), x_eor(t, t, u), and_(u, y1, y0), x_eor(t, t, u),   # t = Maj(X.lo, Y.hi, Y.lo)
                 x_add(t, t, c), x_add(t, t, b)]                                              # t = new hi
        body += ror3(c, t, 28, 34, 39, x1)
        body += [and_(x1, t, y0), and_(u, t, y1), x_eor(x1, x1, u), and_(u, y1, y0), x_eor(x1, x1, u),   # x1 = Maj(t, Y.lo, Y.hi)
                 x_add(x1, x1, c), x_add(x1, x1, a)]                                          # x1 = new lo
        body += [fmov_dx(rd, x1), ins_gp_d(1, rd, t)]
    else:
        return None
    return _wrap(body, (), xs)


# ---------------------------------------------------------------------------------------------------
def _fjcvtzs(w):
    rd, rn = w & 31, (w >> 5) & 31
    if rd == 30:
        return None                                  # x30 carries the thunk's return address
    xs = _xscr({rd}, 8)
    t0, t1, t2, t3, t4, t5, t6, t7 = xs
    vt, = _vscr({rn}, 1)
    body = [fmov_xd(t0, rn),
            frintz_d(vt, rn), fcvtzs_xd(t1, vt),                     # t1 = trunc (exact: no IXC), saturating
            fcmp_d(rn, vt), cset(t4, EQ), cset(t5, VS),             # t4 = exact, t5 = unordered (NaN)
            ubfx_x(t2, t0, 52, 11), ubfx_x(t3, t0, 0, 52), movz_x48(t6, 0x10), orr_x(t3, t3, t6),
            sub_xi(t6, t2, 1075), lslv_x(t3, t3, t6), cmp_xi(t6, 32), csel_x(t3, t3, 31, LO),
            cmp_xi(t0, 0), neg_x(t6, t3), csel_x(t3, t6, t3, LT),   # |v| >= 2^63: (m << (e-1075)) mod 2^32, signed
            cmp_xi(t2, 1086), csel_w(t6, t3, t1, HS),               # t6 = result
            cmp_x_sxtw(t1, t1), cset(t7, NE),                       # t7 = outside int32 range
            orr_x(t2, t5, t7),                                      # t2 = IOC
            orr_x(t3, t4, t2), eor1_x(t3, t3), lsl_xi(t3, t3, 4), orr_x(t3, t3, t2),   # (IXC<<4) | IOC
            mrs_fpsr(t5), orr_x(t5, t5, t3), msr_fpsr(t5),
            bic_x(t4, t4, t2),                                      # Z = exact & !IOC
            cmp_xi(t1, 0), cset(t3, EQ), lsr_xi(t0, t0, 63), and_x(t3, t3, t0), bic_x(t4, t4, t3),   # -0.0 -> Z = 0
            lsl_xi(t4, t4, 30), msr_nzcv(t4)]
    if rd != 31:
        body.append(orr_w(rd, 31, t6))
    return _wrap(body, (vt,), xs)


# ---------------------------------------------------------------------------------------------------
def stub_words_misc(w, thunk_sp=THUNK_SP):
    """thunk_sp = bytes the caller's thunk lowered SP by (144) - only matters for SP-relative operands (LRCPC2 [sp,#imm])."""
    c = classify(w)
    if not c:
        return None
    cl, k = c
    if cl == 'LRCPC2':
        return None if rewrite_inplace(w) else _lrcpc2(w, thunk_sp)
    if cl in ('LRCPC', 'LOR'):
        return None                                 # in-place only
    if cl == 'DotProd':
        return _dotprod(w, k)
    if cl == 'RDM':
        return _rdm(w, k)
    if cl == 'FCMA':
        return _fcma(w, k)
    if cl == 'FHM':
        return _fhm(w, k)
    if cl == 'I8MM':
        return _i8mm(w, k)
    if cl == 'SHA3':
        return _sha3(w, k)
    if cl == 'JSCVT':
        return _fjcvtzs(w)
    if cl == 'BF16':
        try:
            from bf16_stub import stub_words_bf16
        except ImportError:
            return None
        return stub_words_bf16(w, k)
    return None


def plan(w, thunk_sp=THUNK_SP):
    """one-call front end for the rewriter: -> ('inplace', word) | ('stub', [words]) | None (not this module's class / not handled)."""
    r = rewrite_inplace(w)
    if r is not None:
        return ('inplace', r)
    st = stub_words_misc(w, thunk_sp)
    return ('stub', st) if st is not None else None
