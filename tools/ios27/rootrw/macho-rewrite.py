#!/usr/bin/env python3
"""macho-rewrite.py IN OUT [--mode A|T|auto] [--dry-run] [--report FILE]

Offline rewrite of one Mach-O (thin or fat; arm64/arm64e slices) so that no ARMv8.2+ instruction the Exynos M3
lacks is left in executable code: every site becomes `B thunk`; thunk = STR x30,[sp,#-144]!; BL body; LDR x30,[sp],#144;
B site+4 ; one shared body per distinct instruction word (dual A55-native / M3-LL/SC for LSE), in-place LDAPR->LDAR.
The stub generators are the deployed ones (gen/rewrite-grow.py gen_body/thunk_words, fp16_stub, misc_stub, bf16_stub).

Where the thunks/bodies go:
  A  new RX segment __STUBX (LC_SEGMENT_64, nsects 0) appended AFTER the end of the file (file offset = old size
     rounded to 16 KB, vmaddr = end of the last segment rounded to 16 KB). The code signature blob is not touched
     (cdhash/identifier/entitlements unchanged); old pages that were edited, and the new pages beyond codeLimit,
     are not covered by valid page hashes (tolerated: see README.md). Needs 72 bytes of header padding
     (optionally freed by dropping advisory load commands), and the chained-fixups seg_count is kept equal to the
     number of segments (see fix_chained).
  T  no structural change at all: the zero padding at the end of __TEXT (after the last section, inside the
     segment's file range) holds thunks+bodies, like the shared-cache rewrite. Only for files whose need fits.
  auto (default): T when it fits, else A.
  B (insert before __LINKEDIT + re-sign) is NOT implemented (see README.md).
"""
import sys, os, struct, json, argparse, hashlib, importlib.util, collections
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
LC_SEGMENT_64 = 0x19
LC_CODE_SIGNATURE = 0x1d
LC_UUID = 0x1b
LC_DATA_IN_CODE = 0x29
LC_FUNCTION_STARTS = 0x26
LC_SOURCE_VERSION = 0x2a
LC_DYLIB_CODE_SIGN_DRS = 0x2b
LC_LINKER_OPTIMIZATION_HINT = 0x2e
LC_CHAINED_FIXUPS = 0x80000034
PRUNE_ORDER = [LC_SOURCE_VERSION, LC_LINKER_OPTIMIZATION_HINT, LC_DYLIB_CODE_SIGN_DRS, LC_DATA_IN_CODE, LC_FUNCTION_STARTS]
THUNK = 16
LIM = (1 << 27) - 0x10000
STUBX_LC = 72


# ---------------------------------------------------------------------------------------------------
# word classification (shared with the deployed cache rewriter via rg)
_state = {}


def init():
    if _state:
        return _state
    rg.inscope_thunk_mask(np.zeros(1, '<u4'))            # builds rg._HANDLED / rg._INPL
    handled = rg._HANDLED
    inpl = dict(rg._INPL)
    w2m = {}
    for sub in ('root', 'cache'):
        for l in open(os.path.join(GEN, 'oracle', sub, 'all.txt')):
            p = l.rstrip('\n').split('\t')
            if len(p) >= 2:
                try:
                    w2m[int(p[0], 16)] = ' '.join(x.strip() for x in p[1:] if x.strip())
                except ValueError:
                    pass
    unk = set()
    for sub in ('root', 'cache'):
        fn = os.path.join(GEN, 'oracle', sub, 'a53unk.txt')
        if os.path.exists(fn):
            unk.update(int(x, 16) for x in open(fn).read().split())
    _state.update(handled=handled, inpl=inpl, inpl_keys=np.array(sorted(inpl), dtype='<u4'), w2m=w2m,
                  unk=np.array(sorted(unk), dtype='<u4'), bodies={})
    return _state


def wclass(w):
    mn = init()['w2m'].get(int(w))
    return classify.cls(mn.replace('\t', ' ')) if mn else 'LSE?' if rg.lse_kind(int(w)) else 'UNKNOWN'


def body_for(w):
    st = init()
    if w not in st['bodies']:
        st['bodies'][w] = rg.gen_body(w)
    return st['bodies'][w]


def stub_for(w):
    if rg.lse_kind(w) in (None, 'ldapr'):
        return None
    return rg.stub_words(w)


B = rg.b


def ldapr_to_ldar(w):
    """LDAPR{B,H,W,X} Wt/Xt,[Xn] -> LDAR (same size/Rn/Rt; acquire-consistent is stronger than acquire-RCpc)"""
    if (w & 0x3FFFFC00) != 0x38BFC000:
        return None
    return (w & 0xC0000000) | 0x08DFFC00 | (w & 0x3FF)


def thunk_words(thunk, body, site):
    return rg.thunk_words(thunk, body, site)


# ---------------------------------------------------------------------------------------------------
class Slice:
    """parsed thin arm64 Mach-O slice (bytes)"""

    def __init__(self, m):
        self.m = m
        mg, self.cpu, self.sub, self.ft, self.ncmds, self.szc, self.flags = struct.unpack_from('<IIIIIII', m, 0)
        if mg != 0xfeedfacf:
            raise ValueError('not a 64-bit Mach-O')
        self.lcs = []
        o = 32
        for _ in range(self.ncmds):
            cmd, cs = struct.unpack_from('<II', m, o)
            self.lcs.append([cmd, o, cs])
            o += cs
        assert o == 32 + self.szc, 'sizeofcmds mismatch'
        self.segs = []
        self.secs = []
        for cmd, o, cs in self.lcs:
            if cmd == LC_SEGMENT_64:
                name = m[o + 8:o + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fo, fsz = struct.unpack_from('<QQQQ', m, o + 24)
                mx, ip, ns, fl = struct.unpack_from('<iiII', m, o + 56)
                self.segs.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fo, filesize=fsz, maxprot=mx, initprot=ip, nsects=ns, lc=o))
                for s in range(ns):
                    so = o + 72 + 80 * s
                    sname = m[so:so + 16].rstrip(b'\0').decode()
                    addr, size, off = struct.unpack_from('<QQI', m, so + 32)
                    flags = struct.unpack_from('<I', m, so + 64)[0]
                    self.secs.append(dict(seg=name, name=sname, addr=addr, size=size, off=off, flags=flags))

    def first_content(self):
        offs = [s['off'] for s in self.secs if s['size'] and s['off']]
        return min(offs) if offs else 1 << 60

    def lc_room(self):
        return self.first_content() - (32 + self.szc)

    def lc(self, cmd):
        return [x for x in self.lcs if x[0] == cmd]

    def exec_secs(self):
        return [s for s in self.secs if s['flags'] & 0x80000000 and s['size'] and s['off']]

    def dic_ranges(self):
        r = []
        for _, o, _ in self.lc(LC_DATA_IN_CODE):
            doff, dsz = struct.unpack_from('<II', self.m, o + 8)
            for i in range(dsz // 8):
                a, l, k = struct.unpack_from('<IHH', self.m, doff + 8 * i)
                r.append((a, a + l))
        return r


def find_sites(sl, force_inplace=True):
    """-> sites [(site_addr, foff, word, kind)], inplace [(foff, word, newword)], left [(addr, foff, word, cls)]"""
    st = init()
    sites, inpl, left = [], [], []
    dic = sl.dic_ranges()
    for s in sl.exec_secs():
        n = s['size'] // 4
        w = np.frombuffer(sl.m, dtype='<u4', count=n, offset=s['off'])
        m_h = np.isin(w, st['handled'])
        m_i = np.isin(w, st['inpl_keys']) if len(st['inpl_keys']) else np.zeros(n, bool)
        m_l = np.isin(w, st['unk']) & ((w & 0x3FFFFC00) == 0x38BFC000) & ~m_h
        m_u = np.isin(w, st['unk']) & ~m_h & ~m_i & ~m_l
        for kind, mask in (('T', m_h), ('I', m_i), ('P', m_l), ('L', m_u)):
            for i in np.flatnonzero(mask):
                foff = s['off'] + 4 * int(i)
                if any(a <= foff < b for a, b in dic):
                    continue
                word = int(w[i])
                addr = s['addr'] + 4 * int(i)
                if kind == 'T':
                    sites.append((addr, foff, word))
                elif kind == 'I':
                    inpl.append((foff, word, st['inpl'][word], addr))
                elif kind == 'P':
                    inpl.append((foff, word, ldapr_to_ldar(word), addr))
                else:
                    left.append((addr, foff, word, wclass(word)))
    sites.sort()
    return sites, inpl, left


# ---------------------------------------------------------------------------------------------------
def plan_stub(sites, base):
    """Lay out bodies, thunks and per-site stubs from `base`. Returns (blob bytes, records, left)
    records: (site_addr, foff, word, type, thunk_addr, body_addr)."""
    bodies = {}
    need = []
    for site, foff, word in sites:
        bw = body_for(word)
        if bw is not None:
            bodies.setdefault(word, bw)
            need.append((site, foff, word, 'T'))
        else:
            sw = stub_for(word)
            if sw is not None:
                need.append((site, foff, word, 'S'))
            else:
                need.append((site, foff, word, 'X'))
    out = bytearray()
    baddr = {}
    for w in sorted(bodies):
        pad = (-(base + len(out))) % 16
        out += b'\0' * pad
        baddr[w] = base + len(out)
        out += struct.pack('<%dI' % len(bodies[w]), *bodies[w])
    recs, left = [], []
    for site, foff, word, t in need:
        if t == 'T':
            a = base + len(out)
            out += struct.pack('<4I', *thunk_words(a, baddr[word], site))
            recs.append((site, foff, word, 'T', a, baddr[word]))
        elif t == 'S':
            sw = list(stub_for(word))
            a = base + len(out)
            sw.append(B(site + 4 - (a + 4 * len(sw))))
            out += struct.pack('<%dI' % len(sw), *sw)
            recs.append((site, foff, word, 'S', a, a))
        else:
            left.append((site, foff, word, wclass(word)))
    return bytes(out), recs, left


def text_pad_region(sl):
    """T mode: [start, end) VM range of zero padding at the end of __TEXT (after the last section)."""
    seg = [s for s in sl.segs if s['name'] == '__TEXT']
    if not seg:
        return None
    seg = seg[0]
    secs = [s for s in sl.secs if s['seg'] == '__TEXT']
    end = max([s['addr'] + s['size'] for s in secs] + [seg['vmaddr']])
    lim = seg['vmaddr'] + min(seg['vmsize'], seg['filesize'])
    s0 = (end + 63) & ~63
    if s0 >= lim:
        return None
    fo = seg['fileoff'] + (s0 - seg['vmaddr'])
    v = np.frombuffer(sl.m, dtype=np.uint8, count=lim - s0, offset=fo)
    nz = np.flatnonzero(v)
    if len(nz):
        s0 = (s0 + int(nz[-1]) + 64) & ~63
    return (s0, lim) if lim - s0 >= 64 else None


# ---------------------------------------------------------------------------------------------------
def fix_chained(m, nseg_new):
    """Make LC_DYLD_CHAINED_FIXUPS' dyld_chained_starts_in_image.seg_count == nseg_new (= old + 1) without moving any
    byte of __LINKEDIT: (1) use slack after the seg_info_offset[] array, else (2) put starts_in_image 4 bytes earlier
    (into the 4 pad bytes between the 28-byte header and starts_offset=32) and re-base the entries.
    Returns the method string; raises ValueError when neither works."""
    lcs = []
    o = 32
    ncmds = struct.unpack_from('<I', m, 16)[0]
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', m, o)
        lcs.append((cmd, o))
        o += cs
    fx = [o for c, o in lcs if c == LC_CHAINED_FIXUPS]
    if not fx:
        return 'no-chained-fixups'
    fo, fsz = struct.unpack_from('<II', m, fx[0] + 8)
    ver, so, io, sy, ic, ifmt, sfmt = struct.unpack_from('<7I', m, fo)
    base = fo + so
    segc = struct.unpack_from('<I', m, base)[0]
    if segc == nseg_new:
        return 'already'
    assert segc == nseg_new - 1, (segc, nseg_new)
    offs = list(struct.unpack_from('<%dI' % segc, m, base + 4))
    nz = [x for x in offs if x]
    limit = min(nz + [io - so, sy - so]) if (nz or True) else 0
    end_new = 4 + 4 * (segc + 1)
    if limit >= end_new and not any(m[base + 4 + 4 * segc:base + end_new]):
        struct.pack_into('<I', m, base, segc + 1)
        struct.pack_into('<I', m, base + 4 + 4 * segc, 0)
        return 'slack'
    if so >= 32 and so - 4 >= 28 and not any(m[fo + so - 4:fo + so]) and so >= 4 and nz and True:
        nb = base - 4
        new = [segc + 1] + [(x + 4 if x else 0) for x in offs] + [0]
        struct.pack_into('<%dI' % len(new), m, nb, *new)
        struct.pack_into('<I', m, fo + 4, so - 4)
        return 'shift-down'
    raise ValueError('chained fixups: no room for one more seg_info_offset entry')


OUT_OF_SCOPE = ('OOS', 'UNDEF', 'SVE', 'OTHER', 'DC_other')


def set_left(info, left, left_u):
    """left = in-scope sites the generators could not handle (reported as left); left_u = a53-undecodable words
    outside the generators' coverage: in-scope classes -> left, the rest -> counted as out of scope."""
    lf = [(hex(a), '%08x' % w, c, 'no free scratch register / unhandled form') for a, f, w, c in left]
    oos = collections.Counter()
    for a, f, w, c in left_u:
        if c.startswith(OUT_OF_SCOPE):
            oos[c] += 1
        else:
            lf.append((hex(a), '%08x' % w, c, 'outside generator coverage'))
    info['left'] = lf
    info['out_of_scope'] = dict(oos)


def rewrite_slice(buf, mode='auto', prune=True, fixups=True):
    """-> (new slice bytes, info dict)"""
    init()
    sl = Slice(buf)
    info = dict(mode=None, cputype=sl.cpu, cpusubtype=sl.sub & 0xffffff, filetype=sl.ft)
    sites, inpl, left_u = find_sites(sl)
    info['sites_total'] = len(sites) + len(inpl)
    info['inplace'] = len(inpl)
    if not sites and not inpl:
        info['mode'] = 'none'
        set_left(info, [], left_u)
        return bytes(buf), info
    m = bytearray(buf)
    # --- decide placement
    region = text_pad_region(sl)
    placed = None
    if mode in ('T', 'auto') and region:
        blob, recs, left = plan_stub(sites, region[0])
        if region[0] + len(blob) <= region[1]:
            placed = ('T', region[0], blob, recs, left)
    if placed is None:
        if mode == 'T':
            info['mode'] = 'failed'
            info['why'] = 'no room in __TEXT padding'
            return bytes(buf), info
        # A
        vmend = max(s['vmaddr'] + s['vmsize'] for s in sl.segs)
        base = (vmend + PAGE - 1) & ~(PAGE - 1)
        blob, recs, left = plan_stub(sites, base)
        placed = ('A', base, blob, recs, left)
    kind, base, blob, recs, left = placed
    info['mode'] = kind
    # distances
    for site, foff, word, t, a, bd in recs:
        assert abs(a - site) < LIM and abs(bd - a) < LIM and abs(bd - site) < LIM, ('range', hex(site))
    # --- patch sites / inplace
    for site, foff, word, t, a, bd in recs:
        assert struct.unpack_from('<I', m, foff)[0] == word
        struct.pack_into('<I', m, foff, B(a - site))
    for foff, word, nw, addr in inpl:
        assert struct.unpack_from('<I', m, foff)[0] == word
        struct.pack_into('<I', m, foff, nw)
    if kind == 'T':
        seg = [s for s in sl.segs if s['name'] == '__TEXT'][0]
        fo = seg['fileoff'] + base - seg['vmaddr']
        assert not any(m[fo:fo + len(blob)])
        m[fo:fo + len(blob)] = blob
        info['stub'] = dict(kind='T', vmaddr=base, size=len(blob))
        out = bytes(m)
    else:
        dropped = []
        room = sl.lc_room()
        if room < STUBX_LC:
            if not prune:
                info['mode'] = 'failed'
                info['why'] = 'no header room (%d bytes, need %d)' % (room, STUBX_LC)
                return bytes(buf), info
            for cmd in PRUNE_ORDER:
                for lc in sl.lc(cmd):
                    pass
            for cmd in PRUNE_ORDER:
                while room < STUBX_LC:
                    c = [x for x in sl.lcs if x[0] == cmd]
                    if not c:
                        break
                    _, o, cs = c[0]
                    tail = 32 + sl.szc
                    m[o:tail - cs] = m[o + cs:tail]
                    m[tail - cs:tail] = b'\0' * cs
                    sl.lcs.remove(c[0])
                    for x in sl.lcs:
                        if x[1] > o:
                            x[1] -= cs
                    sl.szc -= cs
                    sl.ncmds -= 1
                    room += cs
                    dropped.append(cmd)
            if room < STUBX_LC:
                info['mode'] = 'failed'
                info['why'] = 'no header room after pruning (%d bytes, need %d)' % (room, STUBX_LC)
                return bytes(buf), info
        if any(m[32 + sl.szc:32 + sl.szc + STUBX_LC]):
            raise ValueError('header padding not zero')
        size = (len(blob) + PAGE - 1) & ~(PAGE - 1)
        fileoff = (len(m) + PAGE - 1) & ~(PAGE - 1)
        nseg = len(sl.segs)
        lc = struct.pack('<II16sQQQQiiII', LC_SEGMENT_64, STUBX_LC, b'__STUBX', base, size, fileoff, size, 5, 5, 0, 0)
        o = 32 + sl.szc
        m[o:o + STUBX_LC] = lc
        struct.pack_into('<II', m, 16, sl.ncmds + 1, sl.szc + STUBX_LC)
        fm = 'n/a'
        if fixups:
            fm = fix_chained(m, nseg + 1)
        m += b'\0' * (fileoff - len(m))
        m += blob + b'\0' * (size - len(blob))
        info['stub'] = dict(kind='A', vmaddr=base, size=len(blob), vmsize=size, fileoff=fileoff)
        info['dropped_lcs'] = dropped
        info['fixups'] = fm
        out = bytes(m)
    st = collections.Counter()
    for site, foff, word, t, a, bd in recs:
        st[wclass(word)] += 1
    for foff, word, nw, addr in inpl:
        st[wclass(word) + '(inplace)'] += 1
    info['classes'] = dict(st)
    info['sites_thunk'] = sum(1 for r in recs if r[3] == 'T')
    info['sites_stub'] = sum(1 for r in recs if r[3] == 'S')
    info['distinct_bodies'] = len({r[2] for r in recs if r[3] == 'T'})
    info['stub_bytes'] = len(blob)
    set_left(info, left, left_u)
    return out, info


# ---------------------------------------------------------------------------------------------------
def fat_slices(d):
    mg = struct.unpack('>I', d[:4])[0]
    if mg in (0xcafebabe, 0xcafebabf):
        n = struct.unpack('>I', d[4:8])[0]
        assert mg == 0xcafebabe
        out = []
        for i in range(n):
            cpu, sub, off, size, al = struct.unpack('>IIIII', d[8 + 20 * i:28 + 20 * i])
            out.append((cpu, sub, off, size, al))
        return out
    return None


def rewrite_file(d, mode='auto', **kw):
    """d: file bytes -> (new bytes, [slice infos])"""
    fs = fat_slices(d)
    if fs is None:
        if struct.unpack_from('<I', d, 4)[0] != 0x0100000c:
            return d, [dict(mode='skip', why='not arm64')]
        nb, info = rewrite_slice(d, mode, **kw)
        return nb, [info]
    infos, news = [], []
    for cpu, sub, off, size, al in fs:
        if cpu != 0x0100000c:
            news.append((cpu, sub, d[off:off + size], al))
            infos.append(dict(mode='skip', why='not arm64'))
            continue
        nb, info = rewrite_slice(d[off:off + size], mode, **kw)
        news.append((cpu, sub, nb, al))
        infos.append(info)
    if all(i['mode'] in ('none', 'skip', 'failed') for i in infos):
        return d, infos
    n = len(fs)
    out = bytearray(struct.pack('>II', 0xcafebabe, n))
    hdr_end = 8 + 20 * n
    pos = hdr_end
    table = bytearray()
    body = bytearray()
    for cpu, sub, nb, al in news:
        a = 1 << al
        off = (pos + a - 1) & ~(a - 1)
        body += b'\0' * (off - pos)
        body += nb
        table += struct.pack('>IIIII', cpu, sub, off, len(nb), al)
        pos = off + len(nb)
    out += table
    out += b'\0' * (hdr_end - len(out))
    out += body
    return bytes(out), infos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp')
    ap.add_argument('out')
    ap.add_argument('--mode', default='auto', choices=['A', 'T', 'auto', 'B'])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--report')
    ap.add_argument('--no-prune', action='store_true')
    ap.add_argument('--no-fixups-fix', action='store_true')
    a = ap.parse_args()
    if a.mode == 'B':
        sys.exit('mode B (insert before __LINKEDIT + re-sign) is not implemented; see README.md')
    d = open(a.inp, 'rb').read()
    mode = 'auto' if a.mode == 'auto' else a.mode
    if mode == 'A':
        # force A: pretend T never fits
        global text_pad_region
        text_pad_region = lambda sl: None
    nb, infos = rewrite_file(d, mode, prune=not a.no_prune, fixups=not a.no_fixups_fix)
    print(json.dumps(infos, indent=1, default=str)[:4000])
    if a.report:
        json.dump(infos, open(a.report, 'w'), indent=1, default=str)
    if not a.dry_run:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        open(a.out, 'wb').write(nb)
        os.chmod(a.out, os.stat(a.inp).st_mode & 0o7777)


if __name__ == '__main__':
    main()
