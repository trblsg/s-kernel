# SPDX-License-Identifier: MIT-0
import re
# cls(mnemonic_plus_operands) -> class string. Groups llvm full-feature disassembly into the
# ARMv8.1+ feature classes the Exynos M3 (ARMv8.0) lacks. Half-precision (FEAT_FP16) is detected
# by an h-register operand or a .8h/.4h/.2h vector suffix; base NEON (.4s/.2d) stays v8.0.
def cls(mn):
    if not mn or mn=='<unknown>': return 'UNDEF(v8.0-undef)'
    first=mn.split()[0]
    b=first.split('.')[0]
    suf=first.split('.')[-1] if '.' in first else ''
    ops=mn[len(first):]
    half = suf in ('8h','4h','2h','1h') or re.search(r'(?:^|[ ,\[])h\d', ops) is not None
    # --- exact feature classes ---
    if b=='fjcvtzs': return 'JSCVT_FJCVTZS'
    if b in ('cas','casa','casl','casal','casb','casab','caslb','casalb','cash','casah','caslh','casalh'): return 'LSE_CAS'
    if b in ('casp','caspa','caspl','caspal'): return 'LSE_CASP'
    if b in ('swp','swpa','swpl','swpal','swpb','swpab','swplb','swpalb','swph','swpah','swplh','swpalh'): return 'LSE_SWP'
    if re.match(r'^(ld|st)(add|clr|eor|set|smax|smin|umax|umin)(a|l|al)?(b|h)?$',b): return 'LSE_LDop'
    if b.startswith('ldapr'): return 'LRCPC_LDAPR'
    if b.startswith('ldapur') or b.startswith('stlur'): return 'LRCPC2'
    if b.startswith('ldlar') or b.startswith('stllr'): return 'LOR'
    if b in ('sqrdmlah','sqrdmlsh'): return 'RDM'
    if b in ('sdot','udot'): return 'DotProd'
    if b in ('usdot','sudot','smmla','ummla','usmmla'): return 'I8MM'
    if b in ('bfdot','bfmmla','bfmlalb','bfmlalt','bfcvt','bfcvtn','bfcvtn2','bfmlal','bfmlal2'): return 'BF16'
    if b=='fcadd': return 'FCMA_FCADD'
    if b=='fcmla': return 'FCMA_FCMLA'
    if b in ('frint32x','frint32z','frint64x','frint64z'): return 'FRINTTS'
    if b in ('cfinv','rmif','setf8','setf16'): return 'FlagM'
    if b in ('axflag','xaflag'): return 'FlagM2'
    if b in ('eor3','bcax','rax1','xar','sha512h','sha512h2','sha512su0','sha512su1'): return 'SHA3'
    if b.startswith('sm3') or b.startswith('sm4'): return 'SM'
    if b in ('fmlal','fmlal2','fmlsl','fmlsl2') and half: return 'FHM'
    if b=='dc':
        o=ops.lower()
        if 'cvap' in o: return 'DPB_DC_CVAP'
        if 'cvadp' in o: return 'DPB2_DC_CVADP'
        return 'DC_other'
    if half: return 'FP16'
    # SVE/SME/SVE2/SME2/MOPS/v8.8+ -> unreachable on Apple cores (trap on A55 too); out of scope
    return 'OOS:'+b
