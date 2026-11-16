#!/usr/bin/env python3
"""textdata-guard-test.py - self-test of the data-in-__text rules and of the skip helper of the rewrite passes.

Pure Python + numpy, no vendor data: it builds a synthetic section in memory (ordinary code, a table of
undecodable words with entries that have the bit pattern of instructions the passes rewrite, a tiny function between
two runs of zero padding) and checks that
  - rules C and D flag the table entries and not the ordinary code,
  - the tiny function is flagged too (a known false positive) until it is listed as code, and an exact RETAA stays exempt,
  - reviewed data is added to the skip map and reviewed code is taken out; LC_DATA_IN_CODE ranges are data,
  - textdata_idx() (the TEXTDATA_SKIP helper of the passes) returns exactly the indexes of the section that are in the
    map, and rewrite_words() leaves exactly those words alone while rewriting the same patterns elsewhere,
  - the Mach-O mode of the pass (no map) leaves a LDAPR candidate alone whose neighbours cannot be instructions.

  python3 textdata-guard-test.py        (prints "ok", exit 0, when everything holds)
"""
import importlib.util, os, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


G = load('textdata_guard', os.path.join(HERE, 'textdata-guard.py'))
A = load('a64e_rewrite', os.path.join(HERE, '..', '..', '..', 'a64e', 'a64e-rewrite.py'))

BASE = 0x180004000
U = [0x0f0f0f0f, 0x1e1e1e1e, 0x2d2d2d2d, 0x3c3c3c3c]       # stand-ins for words no decoder accepts (the undef list)
SHA_K = 0x78a5636f                                            # SHA-256 round constant; the bit pattern of LDUMAXAH
MASK = 0x1fffffff                                             # bit mask; the bit pattern of FNMSUB H31,H31,H31,H31
LDAPR_W = 0x38bfc3e0                                          # LDAPRB W0, [SP]: a64e-rewrite turns it into LDARB
RETAA = 0xd65f0bff
RET = 0xd65f03c0
CODE = [0xd10083ff, 0xa9007bfd, 0x910003fd, 0xaa0003e1, 0x91000421, 0xf9400020, 0xf9000421, 0x8b010000,
        0xcb010000, 0x91000400, 0xd2800000, 0x52800001, 0xa9407bfd, 0x910083ff, RETAA]      # a 15-instruction function

words, idx = [], {}


def put(name, ws):
    idx[name] = len(words)
    words.extend(ws)


put('code1', CODE)                                                  # function start at its first word
put('table', U + [MASK] + U + [LDAPR_W] + U + [SHA_K] + U)          # data entries between undecodable words
put('code2', CODE[:14] + CODE)                                      # longer than 2 x 8 words: no rule C inside it
put('pad1', [0] * 12)
put('tiny', [0xd2800020, RET])                                      # MOV X0,#1; RET between two runs of zero padding
put('pad2', [0] * 12)
put('code3', CODE + [0xd2800000, RET])
w = np.array(words, dtype='<u4')
n = len(w)
fidx = np.array([idx['code1'], idx['code2'], idx['code3']], dtype=np.int64)    # LC_FUNCTION_STARTS (the tiny one is not listed)
T = idx['table']
table_words = [T + 4, T + 9, T + 14]
tiny = [idx['tiny'], idx['tiny'] + 1]
code_words = [idx['code1'] + i for i in range(len(CODE))] + [idx['code2'] + i for i in range(len(CODE) + 14)] + \
    [idx['code3'] + i for i in range(len(CODE) + 2)]
undef = np.array(sorted(U), dtype='<u4')


def addr(i):
    return BASE + 4 * i


def addrs(a):
    return {int(x) for x in a}


def check(cond, msg):
    if not cond:
        print('FAIL', msg)
        sys.exit(1)


# --- rules C and D
inv = G.undecodable(w, undef)
check(inv[idx['pad1']] and inv[T] and not inv[idx['code1']], 'undecodable()')
c, d = G.rules_cd(w, inv, np.arange(n, dtype=np.int64), fidx)
flag = c | d
check(all(flag[i] for i in table_words), 'a table word is not flagged')
check(not any(flag[i] for i in code_words), 'ordinary code is flagged')
check(all(flag[i] for i in tiny), 'the tiny function between zero padding is a known candidate')

# --- the skip map of the section
skip = G.section_skip(addr(0), w, undef, fidx)
check(skip.dtype == np.uint64 and list(skip) == sorted(skip), 'the map is a sorted uint64 array')
check({addr(i) for i in table_words} <= addrs(skip), 'the map lacks a table word')
check(not {addr(i) for i in code_words} & addrs(skip), 'the map holds ordinary code')
rev = G.section_skip(addr(0), w, undef, fidx, data=[addr(idx['code2'] + 3)], code=[addr(i) for i in tiny])
check(not {addr(i) for i in tiny} & addrs(rev), 'reviewed code is still in the map')
check(addr(idx['code2'] + 3) in addrs(rev) and {addr(i) for i in table_words} <= addrs(rev), 'reviewed data missing from the map')
ret_only = w.copy()
ret_only[idx['tiny'] + 1] = RETAA
check(addr(idx['tiny'] + 1) not in addrs(G.section_skip(addr(0), ret_only, undef, fidx)), 'an exact RETAA must stay out of the map')
dic = G.section_skip(addr(0), w, undef, fidx, ranges=[(addr(idx['code1'] + 2), addr(idx['code1'] + 4))])
check({addr(idx['code1'] + 2), addr(idx['code1'] + 3)} <= addrs(dic), 'a LC_DATA_IN_CODE range is not in the map')
check(addr(idx['code1'] + 1) not in addrs(dic) and addr(idx['code1'] + 4) not in addrs(dic), 'LC_DATA_IN_CODE range too long')

# --- the skip helper of the passes
kept = np.array(sorted((a - BASE) // 4 for a in addrs(skip)), dtype=np.int64)
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, 'skip.npy')
    np.save(p, np.array(sorted(addrs(skip) | {0x170000000, 0x190000000}), dtype=np.uint64))     # other sections' words too
    os.environ['TEXTDATA_SKIP'] = p
    A._TD = None
    keep = A.textdata_idx(addr(0), n)
    check(keep is not None and list(keep) == list(kept), 'textdata_idx: wrong indexes')
    win = A.textdata_idx(addr(T), 12)
    check(list(win) == [i - T for i in kept if T <= i < T + 12], 'textdata_idx: wrong window')
    check(len(A.textdata_idx(0x1a0000000, 100)) == 0, 'textdata_idx: a section that is not in the map')
    del os.environ['TEXTDATA_SKIP']
    A._TD = None
    check(A.textdata_idx(addr(0), n) is None, 'textdata_idx without the variable must be None')

# rewrite_words: with the map, without it
b1, b2, left = w.copy(), w.copy(), []
c1 = A.rewrite_words(b1, kept, left=left)
c2 = A.rewrite_words(b2)
check(b1[T + 9] == LDAPR_W and b2[T + 9] != LDAPR_W, 'the LDAPR-like table word must stay with the map and change without it')
check(c1['ldapr'] == 0 and c2['ldapr'] == 1, 'ldapr counts %r %r' % (c1, c2))
check(c1.get('data_left') == 1 and left == [T + 9], 'data_left %r %r' % (c1, left))
check('data_left' not in c2, 'data_left must not appear without a map')
check(b1[idx['code1'] + 14] == RET and b2[idx['code1'] + 14] == RET, 'RETAA must become RET')
rest = np.ones(n, bool)
rest[kept] = False
check((b1[kept] == w[kept]).all(), 'a mapped word changed')
check((b1[rest] == b2[rest]).all(), 'an unmapped word differs between the two runs')

# Mach-O mode of the pass: no map, candidates with neighbours that cannot be instructions stay
m = np.array([0] * 9 + [LDAPR_W] + [0] * 9 + [LDAPR_W] + [0xd10083ff] * 3, dtype='<u4')
mm, lf = m.copy(), []
cc = A.rewrite_words(mm, np.zeros(0, np.int64), rulec=True, left=lf)
check(mm[9] == LDAPR_W and cc.get('data_left') == 1 and lf == [9], 'rule C inside the pass: %r %r' % (cc, lf))
check(mm[19] != LDAPR_W, 'a LDAPR next to code must be rewritten')
print('ok')
