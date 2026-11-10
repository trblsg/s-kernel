#!/usr/bin/env python3
"""macho-check.py [--stage DIR] [--ref DIR] [--manifest FILE] [--only LISTFILE]

Off-phone checker for the staged tree produced by macho-rewrite.py / run-all.py. It does NOT import macho-rewrite.py:
it has its own Mach-O parser and re-derives every site by diffing the staged file against the untouched reference.
It reuses only the stub GENERATORS (gen/rewrite-grow.py gen_body / thunk_words / stub_words, misc_stub in-place table)
as the specification of what a body must be, plus llvm-objdump (cortex-a53 vs full feature set) as the instruction
oracle. Exit status 0 only if every check passes; every deviation prints a FAIL line.

Checks per file/slice:
  S1 structure: Mach-O header/LC sizes, segments 16 KB aligned, inside the file, vm ranges ascending and disjoint,
     file ranges disjoint, fat slice table sane; `otool -l` parses; any added segment is __STUBX RX nsects 0.
  S2 mode A: LC_CODE_SIGNATURE and the signature bytes unchanged, `codesign -dvvv` prints the SAME CDHash and Identifier,
     chained-fixups seg_count == number of segments and every old entry preserved.
  D  everything outside {sites, load commands, chained-fixups header, appended bytes / T-mode stub bytes} is byte-identical
     to the reference.
  R  every differing word of an executable section is `B thunk` (thunk in the stub region, exactly the thunk words,
     BL body == generator output for the ORIGINAL word taken from the reference, B back to site+4), or a per-site
     stub, or an in-place rewrite equal to the generator's; +-128 MB range; stub region bytes not used are zero;
     in T mode the stub region was zero in the reference and lies outside every section.
  F  frame contract of every distinct shared body: it lowers SP itself before storing; no store lands in
     [entry SP, entry SP+144).
  C  re-census: no in-scope (non OOS/UNDEF) non-ARMv8.0 word is left in any executable section except the sites
     the manifest documents as left; every distinct stub-region word decodes on cortex-a53 except native LSE words
     (the A55 half of dual bodies).
"""
import sys, os, struct, json, argparse, subprocess, re, collections, importlib.util, tempfile, hashlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, 'gen')
sys.path.insert(0, GEN)


def _load(name, path):
    sp = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(sp)
    sp.loader.exec_module(m)
    return m


rg = _load('rewrite_grow', os.path.join(GEN, 'rewrite-grow.py'))
import classify

PAGE = 0x4000
LIM = (1 << 27) - 0x10000
OOS = ('OOS', 'UNDEF', 'SVE', 'OTHER', 'DC_other')
PRUNABLE = {0x2a, 0x2e, 0x2b, 0x29, 0x26}
fails = []


def fail(m):
    fails.append(m)
    if len(fails) <= 200:
        print('FAIL', m, flush=True)


# ------------------------------------------------------------------ own mini parser
def parse(m):
    mg, cpu, sub, ft, nc, szc, fl = struct.unpack_from('<IIIIIII', m, 0)
    if mg != 0xfeedfacf:
        raise ValueError('bad magic %x' % mg)
    lcs = []
    o = 32
    for _ in range(nc):
        if o + 8 > len(m):
            raise ValueError('LC beyond file')
        cmd, cs = struct.unpack_from('<II', m, o)
        if cs < 8 or cs % 8 or o + cs > 32 + szc:
            raise ValueError('bad LC size')
        lcs.append((cmd, o, cs))
        o += cs
    if o != 32 + szc:
        raise ValueError('sizeofcmds mismatch')
    segs, secs = [], []
    for cmd, o, cs in lcs:
        if cmd == 0x19:
            name = m[o + 8:o + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fo, fsz = struct.unpack_from('<QQQQ', m, o + 24)
            mx, ip, ns, fl2 = struct.unpack_from('<iiII', m, o + 56)
            if cs != 72 + 80 * ns:
                raise ValueError('segment LC size')
            segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fo, filesize=fsz, maxprot=mx, initprot=ip, nsects=ns))
            for s in range(ns):
                so = o + 72 + 80 * s
                addr, size, off = struct.unpack_from('<QQI', m, so + 32)
                flags = struct.unpack_from('<I', m, so + 64)[0]
                secs.append(dict(seg=name, name=m[so:so + 16].split(b'\0')[0].decode(), addr=addr, size=size, off=off, flags=flags))
    return dict(cpu=cpu, sub=sub, ft=ft, nc=nc, szc=szc, lcs=lcs, segs=segs, secs=secs)


def fat(d):
    if struct.unpack('>I', d[:4])[0] == 0xcafebabe:
        n = struct.unpack('>I', d[4:8])[0]
        return [struct.unpack('>IIIII', d[8 + 20 * i:28 + 20 * i]) for i in range(n)]
    return None


def slices(d):
    f = fat(d)
    if f is None:
        return [(struct.unpack_from('<I', d, 4)[0], 0, 0, len(d), 0)]
    return f


def check_structure(tag, m, d_len=None):
    p = parse(m)
    segs = p['segs']
    prev_end = 0
    fr = []
    for s in segs:
        if s['vmaddr'] % PAGE or s['vmsize'] % PAGE:
            if s['vmsize'] or s['name'] != '__PAGEZERO':
                fail(f'{tag}: segment {s["name"]} vm {s["vmaddr"]:#x}+{s["vmsize"]:#x} not 16K aligned')
        if s['filesize']:
            if s['fileoff'] % PAGE:
                fail(f'{tag}: segment {s["name"]} fileoff {s["fileoff"]:#x} not 16K aligned')
            if s['fileoff'] + s['filesize'] > len(m):
                fail(f'{tag}: segment {s["name"]} file range beyond slice end')
            if s['filesize'] > s['vmsize']:
                fail(f'{tag}: segment {s["name"]} filesize > vmsize')
            fr.append((s['fileoff'], s['fileoff'] + s['filesize'], s['name']))
        if s['vmsize'] and s['vmaddr'] < prev_end:
            fail(f'{tag}: segment {s["name"]} overlaps/precedes previous vm range')
        if s['vmsize']:
            prev_end = max(prev_end, s['vmaddr'] + s['vmsize'])
    fr.sort()
    for a, b in zip(fr, fr[1:]):
        if a[1] > b[0]:
            fail(f'{tag}: file ranges of {a[2]} and {b[2]} overlap')
    for sec in p['secs']:
        sg = [s for s in segs if s['name'] == sec['seg']][0]
        if sec['size'] and sec['off'] and not (sg['vmaddr'] <= sec['addr'] and sec['addr'] + sec['size'] <= sg['vmaddr'] + sg['vmsize']):
            fail(f'{tag}: section {sec["name"]} outside its segment')
        if sec['size'] and sec['off'] and sec['off'] + sec['size'] > len(m):
            fail(f'{tag}: section {sec["name"]} beyond slice end')
    return p


def codesig(m, p):
    c = [x for x in p['lcs'] if x[0] == 0x1d]
    if not c:
        return None
    off, sz = struct.unpack_from('<II', m, c[0][1] + 8)
    return off, sz


def fixups_info(m, p):
    c = [x for x in p['lcs'] if x[0] == 0x80000034]
    if not c:
        return None
    fo, fsz = struct.unpack_from('<II', m, c[0][1] + 8)
    hdr = struct.unpack_from('<7I', m, fo)
    base = fo + hdr[1]
    segc = struct.unpack_from('<I', m, base)[0]
    offs = struct.unpack_from('<%dI' % segc, m, base + 4)
    return dict(fo=fo, fsz=fsz, hdr=hdr, base=base, segc=segc, offs=offs)


def cs_id(path):
    r = subprocess.run(['codesign', '-dvvv', path], capture_output=True, text=True)
    t = r.stderr + r.stdout
    cd = re.search(r'^CDHash=(\S+)', t, re.M)
    idf = re.search(r'^Identifier=(\S+)', t, re.M)
    return (cd.group(1) if cd else None, idf.group(1) if idf else None)


def otool_ok(path):
    r = subprocess.run(['otool', '-l', path], capture_output=True, text=True)
    return r.returncode == 0 and 'Load command' in r.stdout and 'malformed' not in r.stderr


# ------------------------------------------------------------------ main check
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--stage', default=os.path.join(HERE, 'stage'))
    ap.add_argument('--ref', default=os.path.join(HERE, 'ref'))
    ap.add_argument('--manifest', default=os.path.join(HERE, 'manifest.json'))
    ap.add_argument('--only')
    ap.add_argument('--skip-codesign', action='store_true')
    a = ap.parse_args()
    man = json.load(open(a.manifest)) if os.path.exists(a.manifest) else {'files': {}}
    documented = {p: e for p, e in man['files'].items()}
    rg.inscope_thunk_mask(np.zeros(1, '<u4'))
    handled = np.array(sorted(set(rg._HANDLED.tolist())), dtype='<u4')
    inpl = dict(rg._INPL)
    inpl_keys = np.array(sorted(inpl), dtype='<u4')
    w2m = {}
    for sub in ('root', 'cache'):
        for l in open(os.path.join(GEN, 'oracle', sub, 'all.txt')):
            q = l.rstrip('\n').split('\t')
            if len(q) >= 2:
                try:
                    w2m[int(q[0], 16)] = ' '.join(x.strip() for x in q[1:] if x.strip())
                except ValueError:
                    pass
    unk = set()
    for sub in ('root', 'cache'):
        fn = os.path.join(GEN, 'oracle', sub, 'a53unk.txt')
        if os.path.exists(fn):
            unk.update(int(x, 16) for x in open(fn).read().split())
    unk_arr = np.array(sorted(unk), dtype='<u4')
    handled_set, inpl_set, unk_set = set(handled.tolist()), set(inpl_keys.tolist()), set(unk)
    gb_cache = {}

    def gen_body(w):
        if w not in gb_cache:
            gb_cache[w] = rg.gen_body(w)
        return gb_cache[w]
    unk_cls = {}

    def uclass(w):
        if w not in unk_cls:
            mn = w2m.get(w)
            unk_cls[w] = classify.cls(mn.replace('\t', ' ')) if mn else 'UNKNOWN'
        return unk_cls[w]

    files = []
    for dp, dns, fns in os.walk(a.stage):
        for fn in fns:
            files.append(os.path.join(dp, fn)[len(a.stage):])
    files.sort()
    if a.only:
        sel = set(l.strip() for l in open(a.only) if l.strip())
        files = [f for f in files if f in sel]
    print('files to check', len(files))
    stub_words_all = set()
    bodies_all = {}                          # body words -> original word (distinct bodies for frame check)
    tot = collections.Counter()
    nfile_mode = collections.Counter()
    for fi, path in enumerate(files):
        sp_, rp_ = a.stage + path, a.ref + path
        if not os.path.exists(rp_):
            fail(f'{path}: no reference file')
            continue
        dn, dr = open(sp_, 'rb').read(), open(rp_, 'rb').read()
        doc = documented.get(path, {})
        if os.stat(sp_).st_mode & 0o7777 != os.stat(rp_).st_mode & 0o7777:
            fail(f'{path}: mode bits differ from reference')
        fn_, fr_ = fat(dn), fat(dr)
        if (fn_ is None) != (fr_ is None) or (fn_ and len(fn_) != len(fr_)):
            fail(f'{path}: fat layout kind changed')
            continue
        sn, sr = slices(dn), slices(dr)
        if fn_:
            end = 8 + 20 * len(fn_)
            for (cpu, sub, off, size, al), (cpu0, sub0, off0, size0, al0) in zip(sn, sr):
                if (cpu, sub, al) != (cpu0, sub0, al0):
                    fail(f'{path}: fat arch entry changed')
                if off % (1 << al) or off < end or off + size > len(dn):
                    fail(f'{path}: fat slice off {off:#x} bad (align/overlap/EOF)')
                end = off + size
            if end != len(dn):
                fail(f'{path}: trailing bytes after last fat slice')
        if not a.skip_codesign and not otool_ok(sp_):
            fail(f'{path}: otool -l does not parse')
        file_changed = dn != dr
        file_modes = set()
        for si, ((cpu, sub, off, size, al), (cpu0, sub0, off0, size0, al0)) in enumerate(zip(sn, sr)):
            tag = f'{path}[{si}]'
            m, r = dn[off:off + size], dr[off0:off0 + size0]
            if cpu != 0x0100000c:
                if m != r:
                    fail(f'{tag}: non-arm64 slice modified')
                continue
            try:
                pn, pr = check_structure(tag, m), check_structure(tag + '(ref)', r)
            except ValueError as e:
                fail(f'{tag}: structure: {e}')
                continue
            # ---- which mode?
            nnew = [s for s in pn['segs'] if s['name'] == '__STUBX']
            old_names = [s['name'] for s in pr['segs']]
            if len(pn['segs']) != len(pr['segs']) + len(nnew) or [s['name'] for s in pn['segs'] if s['name'] != '__STUBX'] != old_names:
                fail(f'{tag}: segment list changed other than adding __STUBX')
            if len(nnew) > 1:
                fail(f'{tag}: more than one __STUBX')
                continue
            modeA = bool(nnew)
            # old segments must be unchanged field by field
            for s0, s1 in zip(pr['segs'], [s for s in pn['segs'] if s['name'] != '__STUBX']):
                if s0 != s1:
                    fail(f'{tag}: existing segment {s0["name"]} changed')
            if (pn['cpu'], pn['sub'], pn['ft']) != (pr['cpu'], pr['sub'], pr['ft']) or struct.unpack_from('<I', m, 24)[0] != struct.unpack_from('<I', r, 24)[0]:
                fail(f'{tag}: header cpu/subtype/filetype/flags changed')
            # LC list: old LCs (minus prunable) must appear unchanged in order; then __STUBX
            old_l = [(c, r[o:o + cs]) for c, o, cs in pr['lcs']]
            new_l = [(c, m[o:o + cs]) for c, o, cs in pn['lcs']]
            if modeA:
                if new_l[-1][0] != 0x19 or not new_l[-1][1][8:24].startswith(b'__STUBX'):
                    fail(f'{tag}: __STUBX LC is not last')
                new_l = new_l[:-1]
            j = 0
            dropped = 0
            for c, b in old_l:
                if j < len(new_l) and new_l[j] == (c, b):
                    j += 1
                elif c in PRUNABLE and modeA:
                    dropped += 1
                elif c == 0x80000034 and modeA:
                    # chained fixups LC itself must be unchanged (dataoff/size): handled by equality above; here mismatch
                    fail(f'{tag}: chained-fixups LC changed')
                else:
                    fail(f'{tag}: load command {c:#x} changed/missing')
            if j != len(new_l):
                fail(f'{tag}: extra load commands')
            if modeA and not (not modeA or dropped >= 0):
                pass
            # ---- stub region
            if modeA:
                st = nnew[0]
                if st['maxprot'] != 5 or st['initprot'] != 5 or st['nsects'] != 0:
                    fail(f'{tag}: __STUBX not RX/nsects 0')
                if st['vmaddr'] < max(s['vmaddr'] + s['vmsize'] for s in pr['segs']) or st['fileoff'] < len(r) or st['fileoff'] + st['filesize'] != len(m) or st['vmsize'] != st['filesize']:
                    fail(f'{tag}: __STUBX placement/size wrong')
                sreg = (st['vmaddr'], st['vmaddr'] + st['vmsize'], st['fileoff'] - st['vmaddr'])
                if len(m) < len(r) or m[len(r):st['fileoff']].strip(b'\0'):
                    fail(f'{tag}: gap before __STUBX not zero')
                # code signature + cdhash
                cr, cn = codesig(r, pr), codesig(m, pn)
                if cr != cn:
                    fail(f'{tag}: LC_CODE_SIGNATURE changed')
                elif cr and m[cr[0]:cr[0] + cr[1]] != r[cr[0]:cr[0] + cr[1]]:
                    fail(f'{tag}: signature blob bytes changed')
                # chained fixups
                fr_i, fn_i = fixups_info(r, pr), fixups_info(m, pn)
                if fr_i:
                    if fn_i['segc'] != len(pn['segs']):
                        fail(f'{tag}: chained fixups seg_count {fn_i["segc"]} != segments {len(pn["segs"])}')
                    shift = 4 if fn_i['hdr'][1] == fr_i['hdr'][1] - 4 else 0
                    if shift == 0 and fn_i['hdr'] != fr_i['hdr']:
                        fail(f'{tag}: chained fixups header changed')
                    if shift == 4 and (fn_i['hdr'][0], fn_i['hdr'][2:]) != (fr_i['hdr'][0], fr_i['hdr'][2:]):
                        fail(f'{tag}: chained fixups header changed beyond starts_offset')
                    for i, (o0, o1) in enumerate(zip(fr_i['offs'], fn_i['offs'])):
                        if o1 != (o0 + shift if o0 else 0):
                            fail(f'{tag}: chained fixups entry {i} {o0}->{o1}')
                    if fn_i['offs'][-1] != 0:
                        fail(f'{tag}: new chained fixups entry not 0')
                    fx_allowed = (fr_i['fo'], fr_i['base'] + 4 + 4 * fr_i['segc'])
                    # starts_in_segment payloads must be identical (they did not move)
                    for i, (o0, o1) in enumerate(zip(fr_i['offs'], fn_i['offs'])):
                        if o0:
                            q0 = fr_i['base'] + o0
                            ssz = struct.unpack_from('<I', r, q0)[0]
                            if r[q0:q0 + ssz] != m[fn_i['base'] + o1:fn_i['base'] + o1 + ssz]:
                                fail(f'{tag}: chained fixups starts_in_segment {i} differs')
                else:
                    fx_allowed = (0, 0)
                if not a.skip_codesign:
                    pass
                file_modes.add('A')
            else:
                sreg = None
                fx_allowed = (0, 0)
                file_modes.add('T' if file_changed else 'none')
                if len(m) != len(r):
                    fail(f'{tag}: slice size changed without __STUBX')
            # ---- exec sections of REFERENCE: find all differing words
            execs = [s for s in pr['secs'] if s['flags'] & 0x80000000 and s['size'] and s['off']]
            allowed = np.zeros(len(r), bool)
            hdr_end = 32 + max(pr['szc'], pn['szc'])
            allowed[:hdr_end] = True
            allowed[fx_allowed[0]:fx_allowed[1]] = True
            diffmask = np.frombuffer(m[:len(r)], np.uint8) != np.frombuffer(r, np.uint8)
            if modeA and len(m) < len(r):
                continue
            sites = []
            ranges = []
            for s in execs:
                n = s['size'] // 4
                wr = np.frombuffer(r, '<u4', count=n, offset=s['off'])
                wn = np.frombuffer(m, '<u4', count=n, offset=s['off'])
                dif = np.flatnonzero(wr != wn)
                for i in dif:
                    foff = s['off'] + 4 * int(i)
                    sites.append((s['addr'] + 4 * int(i), foff, int(wr[i]), int(wn[i])))
                    allowed[foff:foff + 4] = True
                # leftover in-scope words (re-census): a53-undecodable words, plus every word the generators handle
                for wsel, name in ((np.isin(wn, unk_arr) | np.isin(wn, handled) | np.isin(wn, inpl_keys), 'unk'),):
                    for i in np.flatnonzero(wsel):
                        w = int(wn[i])
                        c = uclass(w)
                        if c.startswith(OOS) and not ((w in handled_set) or (w in inpl_set)):
                            tot['oos:' + c] += 1
                            continue
                        foff = s['off'] + 4 * int(i)
                        # DATA_IN_CODE ranges are not code
                        dic = [(q[1], q[2]) for q in []]
                        addr = s['addr'] + 4 * int(i)
                        dl = doc.get('slices', [{}] * (si + 1))[si].get('left', []) if doc.get('slices') else []
                        if any(int(x[0], 16) == addr for x in dl):
                            tot['left_documented'] += 1
                            continue
                        # inside a DATA_IN_CODE range?
                        indic = False
                        for cmd, o, cs in pr['lcs']:
                            if cmd == 0x29:
                                doff, dsz = struct.unpack_from('<II', r, o + 8)
                                for k in range(dsz // 8):
                                    x0, l0, k0 = struct.unpack_from('<IHH', r, doff + 8 * k)
                                    if x0 <= foff < x0 + l0:
                                        indic = True
                        if indic:
                            tot['dic'] += 1
                            continue
                        fail(f'{tag}: in-scope word {w:08x} ({c}) left at {addr:#x} and not documented')
            # ---- verify every site
            if sreg is None and sites:
                # T mode region: zero padding after the last section of __TEXT
                tseg = [s for s in pr['segs'] if s['name'] == '__TEXT'][0]
                tend = max([s['addr'] + s['size'] for s in pr['secs'] if s['seg'] == '__TEXT'] + [tseg['vmaddr']])
                sreg = (tend, tseg['vmaddr'] + min(tseg['vmsize'], tseg['filesize']), tseg['fileoff'] - tseg['vmaddr'])
            if not sites:
                if modeA or file_changed and False:
                    fail(f'{tag}: stub segment but no rewritten site')
                if not modeA and m != r:
                    fail(f'{tag}: changed without any site')
                continue

            def rd(va, n):
                fo = va + sreg[2]
                if not (sreg[0] <= va and va + 4 * n <= sreg[1]):
                    raise IndexError(hex(va))
                return list(struct.unpack_from('<%dI' % n, m, fo))
            used = []
            for site, foff, w0, w1 in sites:
                orig = w0
                tot['sites'] += 1
                if (w1 >> 26) == 0x05:                                      # B
                    imm = w1 & 0x3FFFFFF
                    if imm & 0x2000000:
                        imm -= 1 << 26
                    th = site + 4 * imm
                    if abs(th - site) >= LIM:
                        fail(f'{tag}: site {site:#x} thunk out of range')
                        continue
                    try:
                        tw = rd(th, 4)
                    except IndexError:
                        fail(f'{tag}: site {site:#x} B target {th:#x} not inside the stub region')
                        continue
                    if (tw[1] >> 26) == 0x25 and tw[0] == 0xF8000C00 | ((-144) & 0x1ff) << 12 | 31 << 5 | 30:   # BL body
                        bimm = tw[1] & 0x3FFFFFF
                        if bimm & 0x2000000:
                            bimm -= 1 << 26
                        body = th + 4 + 4 * bimm
                        if abs(body - th) >= LIM:
                            fail(f'{tag}: site {site:#x} body out of range')
                            continue
                        exp = rg.thunk_words(th, body, site)
                        if tw != exp:
                            fail(f'{tag}: thunk at {th:#x} words wrong')
                            continue
                        gw = gen_body(orig)
                        if gw is None:
                            fail(f'{tag}: site {site:#x} word {orig:08x} has no generated body (rewritten anyway)')
                            continue
                        try:
                            got = rd(body, len(gw))
                        except IndexError:
                            fail(f'{tag}: body {body:#x} outside the stub region')
                            continue
                        if got != gw:
                            fail(f'{tag}: body at {body:#x} for {orig:08x} differs from generator')
                            continue
                        if gw[-1] != 0xD65F03C0:
                            fail(f'{tag}: body without RET')
                        used.append((th, th + 16, 'T'))
                        used.append((body, body + 4 * len(gw), 'B'))
                        bodies_all[tuple(gw)] = orig
                        stub_words_all.update(tw)
                        stub_words_all.update(gw)
                        tot['sites_thunk'] += 1
                    else:
                        # per-site stub: stub words then B site+4
                        sw = rg.stub_words(orig) if rg.lse_kind(orig) not in (None, 'ldapr') else None
                        if sw is None:
                            fail(f'{tag}: site {site:#x} target is not a thunk and no per-site stub exists')
                            continue
                        exp = list(sw) + [rg.b(site + 4 - (th + 4 * len(sw)))]
                        try:
                            got = rd(th, len(exp))
                        except IndexError:
                            fail(f'{tag}: stub outside region')
                            continue
                        if got != exp:
                            fail(f'{tag}: per-site stub at {th:#x} differs from generator')
                            continue
                        used.append((th, th + 4 * len(exp), 'S'))
                        stub_words_all.update(got)
                        tot['sites_stubbed'] += 1
                else:
                    ex = inpl.get(orig)
                    if ex is None and (orig & 0x3FFFFC00) == 0x38BFC000 and (orig in unk_set):
                        ex = (orig & 0xC0000000) | 0x08DFFC00 | (orig & 0x3FF)       # LDAPR -> LDAR, independent formula
                    if ex is None or ex != w1:
                        fail(f'{tag}: site {site:#x} {orig:08x}->{w1:08x} is neither B-thunk nor a generator in-place rewrite')
                    else:
                        tot['sites_inplace'] += 1
                if not (orig in set() or True):
                    pass
                # the ORIGINAL word must be a word the oracle knows as in-scope
                if not ((orig in handled_set) or (orig in inpl_set) or ((orig & 0x3FFFFC00) == 0x38BFC000 and (orig in unk_set))):
                    fail(f'{tag}: site {site:#x} original {orig:08x} is not a handled in-scope word')
            # ---- stub region occupancy
            used = sorted(set(used))
            for x, y in zip(used, used[1:]):
                if x[1] > y[0]:
                    fail(f'{tag}: overlapping stub intervals {x} {y}')
            lo = min(u[0] for u in used) if used else sreg[0]
            hi = max(u[1] for u in used) if used else sreg[0]
            if modeA:
                lo, hi = sreg[0], sreg[1]
            v = np.frombuffer(m, np.uint8, count=hi - lo, offset=lo + sreg[2]).copy()
            msk = np.ones(len(v), bool)
            for x0, x1, k in used:
                msk[x0 - lo:x1 - lo] = False
            if np.count_nonzero(v[msk]):
                fail(f'{tag}: non-zero bytes in unused stub-region space')
            if not modeA:
                # T mode: region zero in reference, outside every section, only stub bytes differ
                for x0, x1, k in used:
                    fo = x0 + sreg[2]
                    if r[fo:fo + (x1 - x0)].strip(b'\0'):
                        fail(f'{tag}: T-mode stub overwrote non-zero reference bytes at {x0:#x}')
                    for s in pr['secs']:
                        if s['addr'] < x1 and x0 < s['addr'] + s['size']:
                            fail(f'{tag}: T-mode stub inside section {s["name"]}')
                    allowed[fo:fo + (x1 - x0)] = True
                if used:
                    pass
            bad = np.flatnonzero(diffmask & ~allowed)
            if len(bad):
                fail(f'{tag}: {len(bad)} bytes differ from the reference outside sites/LCs/fixups/stubs, first at {bad[0]:#x}')
        for md in file_modes:
            nfile_mode[md] += 1
        if 'A' in file_modes and not a.skip_codesign:
            c0, c1 = cs_id(rp_), cs_id(sp_)
            if c0[0] is None or c0 != c1:
                fail(f'{path}: codesign cdhash/identifier differ {c0} vs {c1}')
        if fi % 40 == 0:
            print('..', fi, '/', len(files), flush=True)
    print('sites verified', dict(tot))
    print('files by mode', dict(nfile_mode))

    # ---- F: frame contract over distinct shared bodies
    bl = sorted(bodies_all.items())
    d = tempfile.mkdtemp(prefix='chk', dir=os.environ.get('TMPDIR', '/tmp'))
    allw, owners = [], []
    for gw, orig in bl:
        allw += list(gw) + [0x0000dead]
        owners.append(orig)
    np.array(allw, dtype='<u4').tofile(d + '/b.bin')
    open(d + '/b.s', 'w').write('.text\n.incbin "b.bin"\n')
    subprocess.check_call(['clang', '-arch', 'arm64', '-c', 'b.s', '-o', 'b.o'], cwd=d)
    odx = subprocess.check_output(['xcrun', '--find', 'llvm-objdump']).decode().strip()
    txt = subprocess.check_output([odx, '-d', 'b.o'], cwd=d).decode().splitlines()
    k = depth = nbad = nsub = 0
    lowered = False
    for l in txt:
        p = l.split('\t')
        if len(p) < 2:
            continue
        ins = p[1].strip()
        ops = p[2].strip() if len(p) > 2 else ''
        if ins == 'udf':
            k += 1
            depth = 0
            lowered = False
            continue
        m_ = re.match(r'sub\s+sp, sp, #(0x[0-9a-f]+|\d+)', ins + ' ' + ops)
        if m_:
            depth += int(m_.group(1), 0)
            lowered = True
            continue
        m_ = re.match(r'add\s+sp, sp, #(0x[0-9a-f]+|\d+)', ins + ' ' + ops)
        if m_:
            depth -= int(m_.group(1), 0)
            continue
        mm = re.search(r'\[sp(?:, #(-?(?:0x[0-9a-f]+|\d+)))?\](!)?(?:, #(-?(?:0x[0-9a-f]+|\d+)))?', ops)
        if mm and (ins.startswith('st')):
            off = int(mm.group(1), 0) if mm.group(1) else 0
            if mm.group(2):
                depth -= off
                off = 0
            rel = off - depth
            width = 32 if ins.startswith('stp') and 'q' in ops.split(',')[0] else (16 if ins.startswith('stp') else (16 if ops.startswith('q') else 8))
            if rel + width > 0 and rel < 144:
                nbad += 1
                if nbad <= 10:
                    print('   FRAME BAD', f'{owners[k]:08x}', ins, ops)
            if mm.group(3):
                depth -= int(mm.group(3), 0)
    print('frame check: distinct bodies', len(owners), 'stores into the thunk frame/red zone:', nbad)
    if nbad:
        fail(f'{nbad} body stores land in [entry SP, +144)')

    # ---- C: decode every distinct stub-region word on cortex-a53
    ws = np.array(sorted(w for w in stub_words_all if w), dtype='<u4')
    ws.tofile(d + '/s.bin')
    open(d + '/s.s', 'w').write('.text\n.incbin "s.bin"\n')
    subprocess.check_call(['clang', '-arch', 'arm64', '-c', 's.s', '-o', 's.o'], cwd=d)
    out = subprocess.check_output([odx, '-d', '--mcpu=cortex-a53', 's.o'], cwd=d).decode()
    unkn = [l for l in out.splitlines() if '<unknown>' in l]
    ufull = np.array([int(l.split(':')[1].split()[0], 16) for l in unkn], dtype='<u4')
    ufull.tofile(d + '/u.bin')
    open(d + '/u.s', 'w').write('.text\n.incbin "u.bin"\n')
    subprocess.check_call(['clang', '-arch', 'arm64', '-c', 'u.s', '-o', 'u.o'], cwd=d)
    of = subprocess.check_output([odx, '-d', 'u.o'], cwd=d).decode().splitlines()
    cl = collections.Counter()
    nbadu = 0
    for l in of:
        p = l.split('\t')
        if len(p) < 3:
            continue
        c = classify.cls(p[1].strip() + ' ' + p[2].strip())
        cl[c] += 1
        if not c.startswith('LSE_'):
            nbadu += 1
            fail(f'stub word undecodable on cortex-a53 and not native LSE: {l}')
    print('stub distinct words', len(ws), 'a53-undecodable', len(unkn), 'by class (native A55 half of dual bodies):', dict(cl))
    # sanity: the decoded set covers nothing unexpected
    import shutil
    shutil.rmtree(d, ignore_errors=True)
    print('FAILS', len(fails))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
