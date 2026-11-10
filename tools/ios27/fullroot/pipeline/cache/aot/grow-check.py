#!/usr/bin/env python3
"""grow-check.py NEW_MAIN REF_MAIN LOG  -- off-phone replay of the shared-region mapping rules + dyld header checks +
rewrite verification for a cache produced by rewrite-grow.py (mappingCount unchanged, TEXT mapping grown).
Exit 0 only if every check passes."""
import sys, os, glob, struct, importlib.util, collections, subprocess
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sp = importlib.util.spec_from_file_location('rg', os.path.join(HERE, 'rewrite-grow.py')); rg = importlib.util.module_from_spec(sp); sp.loader.exec_module(rg)
NEW, REF, LOG = sys.argv[1:4]
PAGE = 0x4000; SR0 = 0x180000000; SRS = 0x200000000   # SHARED_REGION_BASE/SIZE_ARM64 of the fork kernel
fails = []
def fail(m):
    fails.append(m)
    if len(fails) <= 60: print('FAIL', m)
def files(main):
    out = []
    for p in sorted(glob.glob(main + '*')):
        if p.endswith(('.map', '.atlas', '.a2s')): continue
        if not os.path.isfile(p): continue
        with open(p, 'rb') as f: h = f.read(PAGE)
        if h.startswith(b'dyld_v1'): out.append(p)
    return out
nf, rf = files(NEW), files(REF)
assert [os.path.basename(x) for x in nf] == [os.path.basename(x) for x in rf], 'file set differs'
print('files', len(nf))
allm = []; grown = {}
tot_files = 0
for pn, pr in zip(nf, rf):
    n = os.path.basename(pn)
    hn = open(pn, 'rb').read(PAGE); hr = open(pr, 'rb').read(PAGE)
    fsn, fsr = os.path.getsize(pn), os.path.getsize(pr)
    mo, mc, wo, wc, maps, mws = rg.parse_header_maps(hn)
    mor, mcr, wor, wcr, mapsr, mwsr = rg.parse_header_maps(hr)
    if (mo, mc, wo, wc) != (mor, mcr, wor, wcr): fail(f'{n}: header array position/count changed'); continue
    # page 0 may differ only in: mapping_info[0].size, mws[0].size, codeSignatureOffset
    allowed = set()
    G = 0
    if fsn != fsr:
        G = maps[0][1] - mapsr[0][1]
        if G <= 0 or G % PAGE or fsn - fsr != G: fail(f'{n}: growth {G:#x} vs file delta {fsn-fsr:#x}')
        allowed |= set(range(mo + 8, mo + 16)) | set(range(0x28, 0x30))
        if wc: allowed |= set(range(wo + 8, wo + 16))
    diff = [i for i in range(PAGE) if hn[i] != hr[i]]
    bad = [i for i in diff if i not in allowed]
    if bad: fail(f'{n}: header page differs outside the allowed fields at {[hex(i) for i in bad[:6]]}')
    for k in range(mc):
        if k == 0 and G:
            if maps[0][0] != mapsr[0][0] or maps[0][2:] != mapsr[0][2:]: fail(f'{n}: mapping0 start/foff/prot changed')
        elif maps[k] != mapsr[k]: fail(f'{n}: mapping {k} changed')
    if wc:
        for k in range(wc):
            m, w = maps[k], mws[k]
            if (w[0], w[1], w[2], w[6], w[7]) != (m[0], m[1], m[2], m[3], m[4]): fail(f'{n}: mws[{k}] != mapping_info[{k}]')
            if k > 0 or not G:
                if mws[k] != mwsr[k]: fail(f'{n}: mws {k} changed')
            else:
                if mws[0][0] != mwsr[0][0] or mws[0][2:] != mwsr[0][2:]: fail(f'{n}: mws0 other fields changed')
    cso, css = struct.unpack_from('<QQ', hn, 0x28); csor, cssr = struct.unpack_from('<QQ', hr, 0x28)
    # dyld/kernel rule (true for every file of the stock cache): the code signature covers EXACTLY the mapped file ranges
    import csgrow
    with open(pn, 'rb') as ff:
        ff.seek(cso); blobn = ff.read(css)
    try:
        _, _, _, _, cdn = csgrow.parse(blobn)
        mxend = max(m[2] + m[1] for m in maps)
        if cdn['codeLimit'] != mxend or cdn['nCode'] != mxend // PAGE: fail(f'{n}: CodeDirectory codeLimit {cdn["codeLimit"]:#x}/nCode {cdn["nCode"]} != last mapped file offset {mxend:#x}')
    except Exception as e:
        fail(f'{n}: cannot parse code signature: {e!r}')
    if (css != cssr) or cso != csor + G: fail(f'{n}: codeSignature {cso:#x}+{css:#x} vs ref {csor:#x}+{cssr:#x} G={G:#x}')
    if cso + css != fsn: fail(f'{n}: codeSignature end {cso+css:#x} != file size {fsn:#x}')
    for k, (a, s, o, mx, ip) in enumerate(maps):
        for nm, v in (('addr', a), ('size', s), ('foff', o)):
            if v % PAGE: fail(f'{n}: mapping {k} {nm} {v:#x} not 16K aligned')   # kernel sanitize: CHECK_ALIGNED start/size, offset & mask
        if a + s < a or a < SR0 or a + s > SR0 + SRS: fail(f'{n}: mapping {k} outside shared region')
        if o + s > fsn: fail(f'{n}: mapping {k} file range {o:#x}+{s:#x} beyond EOF {fsn:#x}')
        if ip & ~7 or mx & ~(7 | 0x10) and False: fail(f'{n}: mapping {k} init prot {ip:#x}')
        if ip & ~mx & 7: fail(f'{n}: mapping {k} init prot {ip:#x} exceeds max {mx:#x}')
        allm.append((a, a + s, n, k, ip, mx))
    if G:
        s0 = mapsr[0][1]
        # stub payload: nonzero somewhere, TEXT head identical to ref except rewritten words; cs tail identical
        newb = np.memmap(pn, dtype=np.uint8, mode='r'); refb = np.memmap(pr, dtype=np.uint8, mode='r')
        import csgrow
        refblob = bytes(refb[s0:s0 + cssr]); newblob = bytes(newb[s0 + G:s0 + G + css])
        try:
            exp = csgrow.grow_blob(refblob, s0, G, bytes(newb[s0:s0 + G]), cssr)
            if exp != newblob: fail(f'{n}: code-signature blob != reference blob grown by the new pages')
        except AssertionError as e:
            fail(f'{n}: code-signature rebuild assertion {e}')
        pay = np.asarray(newb[s0:s0 + G])
        if not pay.any(): fail(f'{n}: grown region is all zero')
        grown[n] = (maps[0][0] + s0, maps[0][0] + s0 + G, pn)
        if maps[0][4] != 5 or maps[0][3] != 5: fail(f'{n}: grown mapping not RX 5/5')
        tot_files += 1
allm.sort()
for x, y in zip(allm, allm[1:]):
    if x[1] > y[0]: fail(f'mapping overlap {x} / {y}')    # vm_map_enter FIXED -> KERN_NO_SPACE (ENOMEM)
print('mappings', len(allm), 'grown owners', tot_files, 'total grown bytes', sum(e - s for s, e, _ in grown.values()))
# per-process limits: SFM_MAX = 256*8 mappings, 256 files
if len(nf) > 256 or len(allm) > 256 * 8: fail('too many files/mappings')
# main-header subcache table: cacheVMOffset of every entry unchanged (start addresses are unchanged)
print('structural fails so far', len(fails))

# ---- B: rewrite replay ----
c = rg.Cache(NEW, True)
cref = rg.Cache(REF, True)
ext = sorted(grown.values())
def in_ext(a, n):
    for s, e, _ in ext:
        if s <= a and a + n <= e: return True
    p, o, prot, mend = c.loc(a)
    if p is None or not (prot & 4) or a + n > mend: return False      # must be inside ONE executable mapping
    rv = cref.view(a, n)                                               # in-image padding: must have been zero in the reference
    return rv is not None and len(rv) == n and not np.asarray(rv).any()
b = rg.b
ivs = []; nsite = 0; ninpl = 0; ndata = 0; seen_body = {}; word_kinds = collections.Counter()
for line in open(LOG):
    if line[0] == '#': continue
    f = line.split(); site, word = int(f[0], 16), int(f[1], 16)
    if len(f) == 3 and f[2] == 'D':      # data word left alone (TEXTDATA_SKIP): must still hold its stock value
        ndata += 1
        if struct.unpack('<I', bytes(c.view(site, 4)))[0] != word or struct.unpack('<I', bytes(cref.view(site, 4)))[0] != word: fail(f'data site {site:#x} not left alone')
        continue
    nsite += 1
    if f[2] == 'I':
        nw = int(f[3], 16)
        if struct.unpack('<I', bytes(c.view(site, 4)))[0] != nw or struct.unpack('<I', bytes(cref.view(site, 4)))[0] != word: fail(f'inplace site {site:#x} mismatch')
        ninpl += 1; continue
    if f[2] != 'T': fail(f'non-thunk record {line.strip()}'); continue
    th, body = int(f[3], 16), int(f[4], 16)
    cur = struct.unpack('<I', bytes(c.view(site, 4)))[0]
    if cur != b(th - site): fail(f'site {site:#x} not B thunk'); continue
    if struct.unpack('<I', bytes(cref.view(site, 4)))[0] != word: fail(f'site {site:#x} ref word != logged'); continue
    if abs(th - site) >= 1 << 27 or abs(body - th) >= 1 << 27: fail(f'site {site:#x} thunk/body out of B range'); continue
    if not in_ext(th, 16): fail(f'thunk {th:#x} not in a grown extension'); continue
    tw = list(struct.unpack('<4I', bytes(c.view(th, 16))))
    if tw != rg.thunk_words(th, body, site): fail(f'thunk {th:#x} words wrong'); continue
    key = (body, word)
    if key not in seen_body:
        bw = rg.gen_body(word)
        if bw is None: fail(f'no body for {word:08x}'); continue
        got = list(struct.unpack(f'<{len(bw)}I', bytes(c.view(body, 4 * len(bw)))))
        if got != bw: fail(f'body {body:#x} for {word:08x} differs from generator'); continue
        if bw[-1] != 0xD65F03C0: fail(f'body {body:#x} no RET at end'); continue
        if not in_ext(body, 4 * len(bw)): fail(f'body {body:#x} outside grown extension'); continue
        seen_body[key] = 4 * len(bw); ivs.append((body, body + 4 * len(bw)))
    ivs.append((th, th + 16))
# a body for one word may be shared by many thunks: dedupe intervals
ivs = sorted(set(ivs))
for x, y in zip(ivs, ivs[1:]):
    if x[1] > y[0]: fail(f'overlapping stub intervals {x} {y}')
print('data words left alone', ndata)
print('sites replayed', nsite, 'in-place', ninpl, 'distinct bodies', len(seen_body), 'stub intervals', len(ivs))
# unused bytes inside the grown extensions are zero
used_bytes = sum(e - s for s, e in ivs)
tot = 0; nz_unused = 0
for s, e, p in ext:
    v = np.asarray(c.view(s, e - s)); m = np.ones(len(v), bool)
    for a0, a1 in ivs:
        if s <= a0 < e: m[a0 - s:min(a1, e) - s] = False
    nz_unused += int(np.count_nonzero(v[m])); tot += e - s
print('extension bytes', tot, 'used by stubs', used_bytes, 'unused nonzero', nz_unused)
if nz_unused: fail('nonzero bytes in unused extension space')

# ---- B2: stack-frame contract of every distinct body: no store may land in [entry SP, entry SP+144) (x30 slot + caller red zone) ----
import re
bodies = sorted(seen_body.items(), key=lambda kv: kv[0])
dd = os.path.join(HERE, 'stubdec'); os.makedirs(dd, exist_ok=True)
allw = []; owners = []
for (body, word), n in bodies:
    allw += list(struct.unpack(f'<{n // 4}I', bytes(c.view(body, n)))) + [0x0000dead]; owners.append((body, word))
np.array(allw, dtype='<u4').tofile(dd + '/b.bin'); open(dd + '/b.s', 'w').write('.text\n.incbin "b.bin"\n')
subprocess.check_call(['clang', '-arch', 'arm64', '-c', 'b.s', '-o', 'b.o'], cwd=dd)
odx = subprocess.check_output(['xcrun', '--find', 'llvm-objdump']).decode().strip()
txt = subprocess.check_output([odx, '-d', 'b.o'], cwd=dd).decode().splitlines()
k = 0; depth = 0; nbad = 0; STW = re.compile(r'^(st[a-z0-9]*|stp|str[a-z]*|stur[a-z]*)\b')
for l in txt:
    p = l.split('\t')
    if len(p) < 2: continue
    ins = p[1].strip(); ops = p[2].strip() if len(p) > 2 else ''
    if ins == 'udf':
        k += 1; depth = 0; continue
    m = re.match(r'sub\s+sp, sp, #(0x[0-9a-f]+|\d+)', ins + ' ' + ops)
    if m: depth += int(m.group(1), 0); continue
    m = re.match(r'add\s+sp, sp, #(0x[0-9a-f]+|\d+)', ins + ' ' + ops)
    if m: depth -= int(m.group(1), 0); continue
    mm = re.search(r'\[sp(?:, #(-?(?:0x[0-9a-f]+|\d+)))?\](!)?(?:, #(-?(?:0x[0-9a-f]+|\d+)))?', ops)
    if mm and (ins.startswith('st') ):
        off = int(mm.group(1), 0) if mm.group(1) else 0
        if mm.group(2): depth -= off; off = 0        # pre-index: SP moves first
        rel = off - depth                              # relative to the entry SP
        width = 32 if ins.startswith('stp') and 'q' in ops.split(',')[0] else (16 if ins.startswith('stp') else (16 if ops.startswith('q') else 8))
        if rel + width > 0 and rel < 144:
            nbad += 1
            if nbad <= 10: print('   FRAME BAD', hex(owners[k][0]), f'{owners[k][1]:08x}', ins, ops)
        if mm.group(3): depth -= int(mm.group(3), 0)  # post-index
print('body frame check: distinct bodies', len(owners), 'stores into the thunk frame/red zone:', nbad)
if nbad: fail(f'{nbad} body stores land in [entry SP, +144)')

# ---- C: re-census ----
a53unk = set(int(x, 16) for x in open(os.path.join(HERE, 'oracle/cache/a53unk.txt')).read().split())
unk = np.array(sorted(a53unk), dtype='<u4')
w2m = {}
for l in open(os.path.join(HERE, 'oracle/cache/all.txt')):
    p = l.rstrip('\n').split('\t')
    if len(p) >= 2:
        try: w2m[int(p[0], 16)] = p[1].strip()
        except: pass
sys.path.insert(0, os.path.join(HERE, '..', 'ios27-rewrite-all'))
import classify
left = collections.Counter(); leftimg = collections.Counter(); nwords = 0
for path, base, lc, ncmds in c.images():
    for addr, size, _ in rg.text_sections(lc, ncmds):
        v = c.view(addr, size & ~3)
        if v is None: continue
        w = np.asarray(v).view('<u4'); nwords += len(w)
        m = np.isin(w, unk)
        for x, cnt in zip(*np.unique(w[m], return_counts=True)):
            k = classify.cls(w2m.get(int(x), '?')); left[k] += int(cnt); leftimg[os.path.basename(path)] += int(cnt)
print('re-census: text words', nwords, 'a53-undecodable words remaining', sum(left.values()))
for k, v in left.most_common(): print('   ', v, k)
print('   top images', leftimg.most_common(8))
# ---- D: decode every distinct word of every stub with the cortex-a53 oracle ----
ws = set()
for s, e, p in ext:
    v = np.asarray(c.view(s, e - s)).view('<u4'); ws.update(np.unique(v).tolist())
ws.discard(0)
arr = np.array(sorted(ws), dtype='<u4'); d = os.path.join(HERE, 'stubdec'); os.makedirs(d, exist_ok=True)
arr.tofile(d + '/s.bin'); open(d + '/s.s', 'w').write('.text\n.incbin "s.bin"\n')
subprocess.check_call(['clang', '-arch', 'arm64', '-c', 's.s', '-o', 's.o'], cwd=d)
od = subprocess.check_output(['xcrun', '--find', 'llvm-objdump']).decode().strip()
out = subprocess.check_output([od, '-d', '--mcpu=cortex-a53', 's.o'], cwd=d).decode()
unkn = [l for l in out.splitlines() if '<unknown>' in l]
print('stub distinct words', len(arr), 'a53-undecodable', len(unkn), '(expected: only the native-LSE words of the dual A55 path)')
ubad = []
ufull = [int(l.split(':')[1].split()[0], 16) for l in unkn]
open(d + '/u.bin', 'wb').write(np.array(ufull, dtype='<u4').tobytes()); open(d + '/u.s', 'w').write('.text\n.incbin "u.bin"\n')
subprocess.check_call(['clang', '-arch', 'arm64', '-c', 'u.s', '-o', 'u.o'], cwd=d)
of = subprocess.check_output([od, '-d', 'u.o'], cwd=d).decode().splitlines()
cl = collections.Counter()
for l in of:
    p = l.split('\t')
    if len(p) < 3: continue
    k = classify.cls(p[1].strip() + ' ' + p[2].strip()); cl[k] += 1
    if not k.startswith('LSE_'): ubad.append(l)
print('   undecodable stub words by class:', dict(cl))
for l in ubad[:20]: print('   ', l)
if ubad: fail(f'{len(ubad)} non-LSE stub words undecodable on cortex-a53')
print('FAILS', len(fails))
sys.exit(1 if fails else 0)
