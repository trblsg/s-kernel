#!/usr/bin/env python3
"""bf16_stub.py - ARMv8.0 replacements for FEAT_BF16: BFDOT, BFMMLA, BFMLALB/T, BFCVT, BFCVTN{,2}.

Semantics modelled (verified against qemu-system-aarch64 -cpu max, the only BF16 implementation available off-phone;
the M1 has no FEAT_BF16/I8MM):
  BFMLALB/T   = FMLA on the bf16->f32 widened (<<16) operands: standard FPCR behaviour, fused, flags as FMLA.
  BFDOT/BFMMLA (FPCR.EBF == 0 behaviour, the only one the cache's cores define): FPCR rounding/FZ/DN are ignored and no FPSR flag
     is set; behave as  r = RTO(acc + RTO(a0*b0 + a1*b1))  with FZ in/out, default-NaN, round-to-odd (overflow -> inf), the
     bf16 products themselves being exact.  The stub runs the arithmetic with FPCR = DN only (RNE, FZ=0), emulates FZ by hand
     and round-to-odd with a TwoSum error term, then restores FPCR and FPSR.
  BFCVT/BFCVTN: f32 -> bf16 honouring FPCR.RMode/FZ/DN and raising IOC/OFC/UFC/IXC/IDC exactly (integer implementation).
Entry/exit contract = misc_stub (BL body, x30 return, preserves everything but Vd / the status bits the op defines).
These bodies use up to 256 bytes of frame below SP.
"""
import misc_stub as M
from misc_stub import (v3, v2, vperm, vshift, eor_, bic_, orr_, mov_, and_, add_, sub_, fadd_, fmla_, cmeq_, cmeq0_, shl_, ushr_,
                       dup_el, ins_el, movi32, xtl_, trn1_, trn2_, fcvtl_, RET, classify, _vscr, _xscr,
                       sub_sp, add_sp, stp_q, ldp_q, str_q, ldr_q, stp_x, ldp_x, str_x, ldr_x, EQ, NE, HS, LO, LT, csel_w)

MAXFRAME = 288


def _wrap(body, vs=(), xs=()):
    vs, xs = list(vs), list(xs)
    size = (16 * len(vs) + 8 * len(xs) + 15) & ~15
    assert size <= MAXFRAME, size
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


# --------------------------------------------------------------------------------------------- encoders
def fmul_(d, n, m, Q=1):   return v3(Q, 1, 0, 0b11011, d, n, m)
def fsub_(d, n, m, Q=1):   return v3(Q, 0, 2, 0b11010, d, n, m)
def fadd4(d, n, m, Q=1):   return v3(Q, 0, 0, 0b11010, d, n, m)
def fcmeq0_(d, n, Q=1):    return v2(Q, 0, 2, 0b01101, d, n)
def cmtst_(d, n, m, Q=1):  return v3(Q, 0, 2, 0b10001, d, n, m)
def orn_(d, n, m, Q=1):    return v3(Q, 0, 3, 0b00011, d, n, m)
def sshr_(Q, imm, esize, d, n): return vshift(Q, 0, 2 * esize - imm, 0b00000, d, n)
def mvni32(Q, imm8, shift, d):  return 0x2F000400 | Q << 30 | (imm8 >> 5) << 16 | (shift // 8 * 2) << 12 | (imm8 & 31) << 5 | d
def uzp1_(d, n, m, Q=1):   return vperm(Q, 2, 0b001, d, n, m)
def uzp2_(d, n, m, Q=1):   return vperm(Q, 2, 0b101, d, n, m)
def mrs_fpcr(t): return 0xD53B4400 | t
def msr_fpcr(t): return 0xD51B4400 | t
def mrs_nzcv(t): return 0xD53B4200 | t
def msr_nzcv(t): return 0xD51B4200 | t
def mrs_fpsr(t): return 0xD53B4420 | t
def msr_fpsr(t): return 0xD51B4420 | t
def movz_w(d, imm16, sh=0): return 0x52800000 | (sh // 16) << 21 | imm16 << 5 | d
# integer W / X forms
def w_ubfx(d, n, lsb, width): return 0x53000000 | lsb << 16 | (lsb + width - 1) << 10 | n << 5 | d
def w_lsr(d, n, s): return w_ubfx(d, n, s, 32 - s)
def w_lsl(d, n, s): return 0x53000000 | (-s & 31) << 16 | (31 - s) << 10 | n << 5 | d
def w_add(d, n, m): return 0x0B000000 | m << 16 | n << 5 | d
def w_sub(d, n, m): return 0x4B000000 | m << 16 | n << 5 | d
def w_addi(d, n, imm, sh12=0): return 0x11000000 | sh12 << 22 | imm << 10 | n << 5 | d
def w_subi(d, n, imm): return 0x51000000 | imm << 10 | n << 5 | d
def w_and(d, n, m): return 0x0A000000 | m << 16 | n << 5 | d
def w_bic(d, n, m): return 0x0A200000 | m << 16 | n << 5 | d
def w_orr(d, n, m): return 0x2A000000 | m << 16 | n << 5 | d
def w_orr_lsl(d, n, m, s): return 0x2A000000 | m << 16 | s << 10 | n << 5 | d
def x_orr_lsl(d, n, m, s): return 0xAA000000 | m << 16 | s << 10 | n << 5 | d
def w_cmpi(n, imm): return 0x7100001F | imm << 10 | n << 5
def w_cmp(n, m): return 0x6B00001F | m << 16 | n << 5
def w_cset(d, cond): return 0x1A9F07E0 | (cond ^ 1) << 12 | d
def w_mov(d, m): return 0x2A0003E0 | m << 16 | d
def umov_w(d, n, i): return 0x0E003C00 | (i << 3 | 4) << 16 | n << 5 | d
def fmov_ws(d, n): return 0x1E260000 | n << 5 | d
def fmov_sw(d, n): return 0x1E270000 | n << 5 | d
def fmov_dx(d, n): return 0x9E670000 | n << 5 | d
def ins_d1_x(d, n): return 0x4E181C00 | n << 5 | d
HI, LS = 8, 9


# --------------------------------------------------------------------------------------------- BFMLAL
def _bfmlal(w, k):
    T = w >> 30 & 1
    rd, rn = w & 31, (w >> 5) & 31
    el = k.endswith('_el')
    if el:
        idx = (w >> 11 & 1) << 2 | (w >> 21 & 1) << 1 | (w >> 20 & 1)
        rm = w >> 16 & 15
    else:
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 2)
    t1, t2 = vs

    def widen(dst, src):
        if T:
            return [ushr_(1, 16, 32, dst, src), shl_(1, 16, 32, dst, dst)]      # odd bf16 element
        return [shl_(1, 16, 32, dst, src)]                                      # even bf16 element
    body = widen(t1, rn)
    if el:
        body += [dup_el(1, idx << 2 | 2, t2, rm), shl_(1, 16, 32, t2, t2)]
    else:
        body += widen(t2, rm)
    body.append(fmla_(0, rd, t1, t2, 1))
    return _wrap(body, vs)


# --------------------------------------------------------------------------------------------- BFDOT / BFMMLA
def _flush(x, t, kexp, kabs):
    """flush f32 denormals (exp==0) to signed zero, in place."""
    return [and_(t, x, kexp), cmeq0_(2, t, t), and_(t, t, kabs), bic_(x, x, t)]


def fmov_half(d):  # FMOV Vd.4S, #0.5
    return 0x4F00F400 | 3 << 16 | d


def _rto_add(a, b, s, x, y, z, w, kexp, kabs, u1, u2, u3, u4):
    """a = RTO(a + b) per 32-bit lane (round-to-odd, overflow -> inf), in an RNE / FZ=0 / DN environment.  b is destroyed.
    u1,u2 scratch; u3,u4 scratch kept across the call body (overflow-window fix-up)."""
    pre = [and_(u1, a, kexp), cmeq_(2, u1, u1, kexp), and_(u2, b, kexp), cmeq_(2, u2, u2, kexp), orr_(u4, u1, u2),      # u4 = a or b is inf/NaN
           fmov_half(x), fmul_(u1, a, x), fmul_(u2, b, x),                                                           # exact halves
           fadd4(x, u1, u2), fsub_(y, x, u1), fsub_(z, x, y), fsub_(z, u1, z), fsub_(y, u2, y), fadd4(y, z, y),        # x = hs (RNE), y = its error
           eor_(u1, y, x), sshr_(1, 31, 32, u1, u1), fcmeq0_(u2, y), bic_(u1, u1, u2),                               # u1 = error points inward
           and_(z, x, kabs), movi32(1, 0x7f, 24, w), cmeq_(2, u2, z, w), bic_(u2, u2, u1),                            # u2 = |hs| == 2^127 and not inward
           v3(1, 1, 2, 0b00110, u1, z, w), orr_(u1, u1, u2),                                                          # u1 = overflow (|exact| >= 2^128)  [CMHI]
           movi32(1, 1, 0, z), sub_(2, u2, kexp, z), v3(1, 1, 2, 0b00011, u2, kexp, u1),                              # u2 = ovf ? inf : FLT_MAX (BIT)
           bic_(u1, x, kabs), orr_(u3, u2, u1)]                                                                      # u3 = that value with the sum's sign
    main = [fadd4(s, a, b), fsub_(x, s, a), fsub_(y, s, x), fsub_(y, a, y), fsub_(x, b, x), fadd4(x, y, x),   # x = exact error
            eor_(a, x, s), sshr_(1, 31, 32, a, a),                  # a = lanes where sign(e) != sign(s)
            fcmeq0_(b, x),                                         # b = lanes with e == 0
            movi32(1, 1, 0, x), cmtst_(y, s, x),                    # x = 1 ; y = odd mantissa lanes
            and_(z, s, kexp), cmeq_(2, z, z, kexp),                  # z = s is inf/NaN
            orr_(w, y, b), orr_(w, w, z),                            # w = lanes that keep s (odd | exact | non-finite)
            shl_(1, 1, 32, a, a), orr_(a, a, x), bic_(a, a, w), add_(2, a, a, s)]   # a = s + (+-1 on adjusted lanes)
    post = [bic_(z, z, u4), v3(1, 1, 2, 0b00011, a, u3, z)]           # lanes: s overflowed to inf although inputs finite -> u3 (BIT)
    return pre + main + post


def _dotadd(A, N, Mm, R):
    """A = bfdotadd(A, N, Mm) lanewise; N/Mm 32-bit lanes of bf16 pairs (not modified).  R: p q s x y z w kexp kabs."""
    p, q, s, x, y, z, w, kexp, kabs, u1, u2, u3, u4 = R
    b = [shl_(1, 16, 32, p, N), ushr_(1, 16, 32, q, N), shl_(1, 16, 32, q, q),
         shl_(1, 16, 32, s, Mm), ushr_(1, 16, 32, x, Mm), shl_(1, 16, 32, x, x)]
    for r in (p, q, s, x):
        b += _flush(r, y, kexp, kabs)
    b += [fmul_(p, p, s), fmul_(q, q, x)]
    b += _flush(p, y, kexp, kabs) + _flush(q, y, kexp, kabs)
    b += _rto_add(p, q, s, x, y, z, w, kexp, kabs, u1, u2, u3, u4)                   # p = RTO(p0 + p1)
    b += _flush(p, y, kexp, kabs)                                    # tiny sums are flushed (FZ output)
    b += _flush(A, y, kexp, kabs)
    b += _rto_add(A, p, s, x, y, z, w, kexp, kabs, u1, u2, u3, u4)                   # A = RTO(A + p)
    b += _flush(A, y, kexp, kabs)
    return b


def _bfdot(w, k):
    Q = w >> 30 & 1
    rd, rn = w & 31, (w >> 5) & 31
    el = k.endswith('_el')
    if el:
        idx = (w >> 11 & 1) << 1 | (w >> 21 & 1)
        rm = (w >> 20 & 1) << 4 | (w >> 16 & 15)
    else:
        rm = w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 14)
    A, p, q, s, x, y, z, w_, kexp, kabs, mt, nt, u3, u4 = vs
    x1, x2, x3 = _xscr(set(), 3)
    body = [mrs_fpcr(x1), mrs_fpsr(x2), movz_w(x3, 0x0200, 16), msr_fpcr(x3),
            movi32(1, 0xff, 24, kexp), ushr_(1, 1, 32, kexp, kexp), mvni32(1, 0x80, 24, kabs), mov_(A, rd)]
    m = rm
    if el:
        body.append(dup_el(1, idx << 3 | 4, mt, rm)); m = mt
    body += _dotadd(A, rn, m, (p, q, s, x, y, z, w_, kexp, kabs, mt, nt, u3, u4))
    body += [mov_(rd, A, Q), msr_fpsr(x2), msr_fpcr(x1)]
    return _wrap(body, vs, (x1, x2, x3))


def _bfmmla(w, k):
    rd, rn, rm = w & 31, (w >> 5) & 31, w >> 16 & 31
    vs = _vscr({rd, rn, rm}, 14)
    A, p, q, s, x, y, z, w_, kexp, kabs, nt, mt, u3, u4 = vs
    x1, x2, x3 = _xscr(set(), 3)
    R = (p, q, s, x, y, z, w_, kexp, kabs, mt, nt, u3, u4)
    body = [mrs_fpcr(x1), mrs_fpsr(x2), movz_w(x3, 0x0200, 16), msr_fpcr(x3),
            movi32(1, 0xff, 24, kexp), ushr_(1, 1, 32, kexp, kexp), mvni32(1, 0x80, 24, kabs), mov_(A, rd)]
    body += [trn1_(2, nt, rn, rn), uzp1_(mt, rm, rm)] + _dotadd(A, nt, mt, R)
    body += [trn2_(2, nt, rn, rn), uzp2_(mt, rm, rm)] + _dotadd(A, nt, mt, R)
    body += [mov_(rd, A, 1), msr_fpsr(x2), msr_fpcr(x1)]
    return _wrap(body, vs, (x1, x2, x3))


# --------------------------------------------------------------------------------------------- BFCVT
def _cvt_lane(a, r, F, fpcr, T):
    """r = bf16(f32 bits in a), F |= FPSR bits.  a,r W regs; T = >= 11 scratch X regs (distinct from a,r,F,fpcr)."""
    k, b, isnan, fz, t, hi, frac, inc, sign, rm, u = T[:11]
    o = []
    o += [movz_w(k, 0x7f80, 16), w_ubfx(b, a, 0, 31), w_cmp(b, k), w_cset(isnan, HI)]               # isnan = |x| > inf
    o += [w_ubfx(fz, fpcr, 24, 1), w_lsr(t, b, 23), w_cmpi(t, 0), w_cset(t, EQ), w_cmpi(b, 0), w_cset(u, NE), w_and(t, t, u)]    # t = denormal input
    o += [w_and(fz, fz, t)]                                                                         # fz = flush this input
    o += [w_lsr(u, a, 31), w_lsl(u, u, 31), w_cmpi(fz, 0), csel_w(a, u, a, NE)]                      # a = flushed ? +-0 : a
    o += [w_ubfx(frac, a, 0, 16), w_lsr(hi, a, 16), w_lsr(sign, a, 31)]
    o += [w_cmpi(frac, 0), w_cset(rm, NE), w_bic(rm, rm, isnan)]                                     # rm = inexact (non-NaN)
    o += [w_ubfx(inc, hi, 0, 1), w_add(inc, inc, frac), w_addi(inc, inc, 8, 1), w_subi(inc, inc, 1), w_lsr(inc, inc, 16)]   # RNE
    o += [w_ubfx(u, fpcr, 22, 2)]
    o += [w_bic(frac, rm, sign), w_cmpi(u, 1), csel_w(inc, frac, inc, EQ)]                           # RP
    o += [w_and(frac, rm, sign), w_cmpi(u, 2), csel_w(inc, frac, inc, EQ)]                           # RM
    o += [w_cmpi(u, 3), csel_w(inc, 31, inc, EQ)]                                                    # RZ
    o += [w_add(a, hi, inc)]                                                                         # a = rounded result
    o += [w_ubfx(u, a, 7, 8), w_cmpi(u, 255), w_cset(u, EQ), w_cmp(b, k), w_cset(frac, NE), w_and(u, u, frac), w_bic(u, u, isnan)]   # u = overflow
    o += [w_ubfx(frac, hi, 6, 1), w_bic(frac, isnan, frac), w_orr(F, F, frac)]                       # IOC: NaN with quiet bit clear
    o += [w_lsl(frac, u, 2), w_orr(F, F, frac)]                                                      # OFC
    o += [w_bic(frac, t, fz), w_and(frac, frac, rm), w_lsl(frac, frac, 3), w_orr(F, F, frac)]        # UFC: denormal input, not flushed, inexact
    o += [w_orr(frac, rm, u), w_lsl(frac, frac, 4), w_orr(F, F, frac)]                               # IXC
    o += [w_lsl(frac, fz, 7), w_orr(F, F, frac)]                                                     # IDC
    o += [w_ubfx(u, fpcr, 25, 1), movz_w(frac, 0x7fc0), w_cmpi(u, 0), movz_w(t, 0x40), w_orr(t, t, hi), csel_w(t, frac, t, NE)]   # t = NaN result
    o += [w_cmpi(isnan, 0), csel_w(r, t, a, NE)]
    return o


def _bfcvt(w, k):
    rd, rn = w & 31, (w >> 5) & 31
    xs = list(range(9, 18)) + list(range(0, 9))
    F, fpcr, a, r = xs[0], xs[1], xs[2], xs[3]
    T = xs[4:15]
    NZ = xs[16]
    body = [mrs_fpcr(fpcr), 0x2A1F03E0 | F, mrs_nzcv(NZ)]                                         # F = 0, save NZCV
    if k == 'bfcvt':
        body += [fmov_ws(a, rn)] + _cvt_lane(a, r, F, fpcr, T) + [fmov_sw(rd, r)]
        used = xs[:15]
    else:
        R64 = xs[15]
        body.append(0xAA1F03E0 | R64)                                                              # R64 = 0
        for i in range(4):
            body += [umov_w(a, rn, i)] + _cvt_lane(a, r, F, fpcr, T) + [x_orr_lsl(R64, R64, r, 16 * i)]
        body.append(ins_d1_x(rd, R64) if (w >> 30 & 1) else fmov_dx(rd, R64))                      # BFCVTN2 inserts the upper half
        used = xs[:16]
    body += [mrs_fpsr(T[0]), w_orr(T[0], T[0], F), msr_fpsr(T[0]), msr_nzcv(NZ)]
    used = xs[:17]
    return _wrap(body, (), used)


def stub_words_bf16(w, k):
    if k in ('bfmlal', 'bfmlal_el'):
        return _bfmlal(w, k)
    if k in ('bfdot', 'bfdot_el'):
        return _bfdot(w, k)
    if k == 'bfmmla':
        return _bfmmla(w, k)
    if k in ('bfcvt', 'bfcvtn'):
        return _bfcvt(w, k)
    return None
