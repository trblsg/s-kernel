#!/usr/bin/env python3
"""textdata-guard.py - keep the rewrite passes away from DATA words inside __text (and find the ones that were not kept away).

The rewrite passes of the cache pipeline (a64e-rewrite, lse-rewrite, a64e-rewrite2, rewrite-grow) look at every 32-bit
word of the pure-instruction sections and rewrite the ones whose bit pattern is an instruction the target CPU lacks.
Hand-written assembly keeps tables in the same sections (bit masks, polynomial coefficients, hash round constants, DCT
tables, ident strings). A table entry with the bit pattern of such an instruction was rewritten like one: 0x1fffffff, a
bit mask, is also FNMSUB H31,H31,H31,H31; 0x78a5636f, a SHA-256 round constant, is also LDUMAXAH. The entry then holds
a branch opcode and the function that reads the table computes garbage (silently: nothing traps).

Two rules decide that a word is "not provably an instruction". They need only "can any A64 decoder decode this word"
(--undef: the words llvm-objdump cannot decode with every feature on, one hex word per line, as oracle.sh writes them;
all-zero-high-half words count as undecodable too):

  rule C  an undecodable word within 8 words before AND within 8 words after the word;
  rule D  walking back from the word over decodable words, the first other thing met is an undecodable word,
          not the function's start (LC_FUNCTION_STARTS) and not an instruction that ends a block (B, BR, RET, BRK...).
          Code is entered at a function start or after such an instruction; a table entry sits behind table entries.

Words whose stock value is an exact pointer-authentication opcode (RETAA/RETAB, BRAA.., BLRAA.., PAC*/AUT*/XPAC*, the
hint space) are exempt (reported by scan only with --pac): a table word is never exactly one of those by accident often
enough to matter, and code that follows vendor-specific instructions the decoder does not know would flood the report.

The skip map (mode map) is the set of addresses of the words the passes must not rewrite: every word of every
pure-instruction section of the stock cache where rule C or D holds (and which is not an exact pointer-auth opcode), or
which lies in an LC_DATA_IN_CODE range of its image; minus the addresses a person reviewed as code, plus the ones
reviewed as data. A word the passes would not rewrite anyway is harmless in the map. The passes read it from the
environment variable TEXTDATA_SKIP (an .npy file) and leave those words alone.

  textdata-guard.py map    STOCK_MAIN --undef FILE [--reviewed LIST] --out skip.npy
  textdata-guard.py verify STOCK_MAIN NEW_MAIN --map skip.npy              exit 1 if a mapped word differs from the stock
  textdata-guard.py scan   STOCK_MAIN NEW_MAIN --undef FILE [--pac] > candidates.tsv
  textdata-guard.py check  STOCK_MAIN NEW_MAIN --undef FILE [--reviewed LIST]   exit 1 if scan finds a word not in LIST
  textdata-guard.py apply  NEW_CACHE_DIR LIST [--dry-run] [--revert]      put reviewed data words back in an old cache

scan prints: image, section, address, stock word, rewritten word, rules, context (stock words -4..+4). It is the review
tool: a person reads the candidates once per cache build (tiny functions between two literal pools are flagged too and are
code), and the ones that are data go to LIST as "VADDR DAMAGED STOCK note" (plus "VADDR code note" lines for the reviewed
code candidates, so that map keeps them rewritten and check stays quiet). With the map in use the data words never change;
apply (writes STOCK back; idempotent; refuses a cache whose words are neither value) is for caches built without it.
Addresses and words of a vendor cache are that vendor's: keep LIST and the map out of public repositories.
"""
import argparse, bisect, glob, os, struct, sys
import numpy as np


class Cache:
    def __init__(self, main):
        self.maps, self.files = [], {}
        for p in sorted(glob.glob(main + '*')):
            if p.endswith(('.map', '.atlas', '.a2s', '.symbols')):
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
                    self.maps.append((a, s, p, o))
            self.files[p] = np.memmap(p, dtype=np.uint8, mode='r')
        self.maps.sort()
        self._starts = [m[0] for m in self.maps]
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

    def view(self, addr, n):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0:
            a, s, p, o = self.maps[i]
            if addr < a + s:
                return self.files[p][o + addr - a:o + addr - a + n]
        return None

    def images(self):
        for im, path in zip(self.imgs, self.paths):
            mh = self.view(im[0], 32)
            if mh is None or struct.unpack_from('<I', bytes(mh))[0] != 0xfeedfacf:
                continue
            ncmds, sz = struct.unpack_from('<II', bytes(mh), 16)
            yield path, im[0], bytes(self.view(im[0] + 32, sz)), ncmds


def load_commands(lc, ncmds):
    """-> (pure-instruction sections [(name, addr, size)], (LINKEDIT vmaddr, fileoff), (function starts off, size),
    (data-in-code off, size))"""
    sects, linkedit, fstarts, dic = [], None, None, None
    off = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            name = lc[off + 8:off + 24].split(b'\0')[0]
            vmaddr, _, fileoff = struct.unpack_from('<QQQ', lc, off + 24)
            if name == b'__LINKEDIT':
                linkedit = (vmaddr, fileoff)
            for s in range(struct.unpack_from('<I', lc, off + 64)[0]):
                so = off + 72 + s * 80
                addr, size = struct.unpack_from('<QQ', lc, so + 32)
                if struct.unpack_from('<I', lc, so + 64)[0] & 0x80000000 and size >= 4:
                    sects.append((lc[so:so + 16].split(b'\0')[0].decode(), addr, size))
        elif cmd == 0x26:
            fstarts = struct.unpack_from('<II', lc, off + 8)
        elif cmd == 0x29:                                  # LC_DATA_IN_CODE: linkedit_data_command
            dic = struct.unpack_from('<II', lc, off + 8)
        off += csz
    return sects, linkedit, fstarts, dic


def function_starts(cache, base, linkedit, fstarts):
    if not linkedit or not fstarts or not fstarts[1]:
        return np.zeros(0, np.int64)
    v = cache.view(linkedit[0] + fstarts[0] - linkedit[1], fstarts[1])
    if v is None:
        return np.zeros(0, np.int64)
    buf, out, a, i = bytes(v), [], base, 0
    while i < len(buf):
        d = sh = 0
        while True:
            b = buf[i]
            i += 1
            d |= (b & 0x7f) << sh
            sh += 7
            if not b & 0x80 or i >= len(buf):
                break
        if d == 0:
            break
        a += d
        out.append(a)
    return np.array(out, dtype=np.int64)


def data_in_code(cache, base, linkedit, dic):
    """[(start address, end address)] of the LC_DATA_IN_CODE ranges of an image: entries are {u32 offset from the
    image's mach header, u16 length, u16 kind}, the blob lives in __LINKEDIT"""
    if not linkedit or not dic or not dic[1]:
        return []
    v = cache.view(linkedit[0] + dic[0] - linkedit[1], dic[1])
    if v is None:
        return []
    out = []
    for off, ln, _kind in struct.iter_unpack('<IHH', bytes(v)[:dic[1] // 8 * 8]):
        if ln:
            out.append((base + off, base + off + ln))
    return out


def is_pac(w):
    """exact pointer-authentication opcodes (what a64e-rewrite turns into their plain forms)"""
    return ((w == 0xD65F0BFF) | (w == 0xD65F0FFF) | ((w & 0xFEFFF800) == 0xD61F0800) | ((w & 0xFEFFF800) == 0xD63F0800) |
            ((w & 0xFFFFF000) == 0xD5032000) | ((w & 0xFFFFF000) == 0xD5033000) | ((w & 0xFFFFF000) == 0xD5034000) |
            ((w & 0xFFFF0000) == 0xDAC10000))


def block_end(w):
    return (((w & 0xFC000000) == 0x14000000) | ((w & 0xFFFFFC1F) == 0xD61F0000) | ((w & 0xFFFFFC1F) == 0xD65F0000) |
            ((w & 0xFEFFF800) == 0xD61F0800) | ((w & 0xFFFFFBFF) == 0xD65F0BFF) | ((w & 0xFFE0001F) == 0xD4200000) |
            ((w & 0xFFFFFBFF) == 0xD69F0BFF) | (w == 0xD69F03E0))


def undecodable(w, undef):
    """bool array: words no decoder accepts (in the oracle's list), or with an all-zero high half"""
    inv = w < 0x10000
    if len(undef):
        i = np.searchsorted(undef, w)
        i[i >= len(undef)] = 0
        inv |= undef[i] == w
    return inv


def rules_cd(w, inv, d, fidx):
    """rules C and D for the words at indexes d of the section words w (stock values); inv = undecodable(w);
    fidx = function start indexes (sorted, may lie outside the section). -> (C bool array, D bool array)"""
    n = len(w)
    cs = np.concatenate(([0], np.cumsum(inv)))
    c = ((cs[d] - cs[np.maximum(d - 8, 0)]) >= 1) & ((cs[np.minimum(d + 9, n)] - cs[np.minimum(d + 1, n)]) >= 1)
    brk = inv | block_end(w)
    last = np.maximum.accumulate(np.where(brk, np.arange(n, dtype=np.int64), -1))
    j = np.where(d > 0, last[np.maximum(d - 1, 0)], -1)
    k = np.searchsorted(fidx, d, side='right') - 1
    fstart = np.maximum(np.where(k >= 0, fidx[np.maximum(k, 0)], 0), 0) if len(fidx) else np.zeros(len(d), np.int64)
    dd = (j >= fstart) & (j >= 0) & inv[np.maximum(j, 0)]
    return c, dd


def scan(stock, new, undef, pac=False):
    S, R = Cache(stock), Cache(new)
    for path, base, lc, ncmds in S.images():
        sects, linkedit, fst, _ = load_commands(lc, ncmds)
        fsa = function_starts(S, base, linkedit, fst)
        for sn, addr, size in sects:
            sv, rv = S.view(addr, size & ~3), R.view(addr, size & ~3)
            if sv is None or rv is None or len(sv) != len(rv):
                continue
            w, r = np.frombuffer(sv, '<u4'), np.frombuffer(rv, '<u4')
            d = np.flatnonzero(w != r)
            if not pac and len(d):
                d = d[~is_pac(w[d])]
            if not len(d):
                continue
            c, dd = rules_cd(w, undecodable(w, undef), d, (fsa - addr) // 4)
            for i in np.flatnonzero(c | dd):
                x = int(d[i])
                yield (path, sn, addr + 4 * x, int(w[x]), int(r[x]), ('C' if c[i] else '') + ('D' if dd[i] else ''),
                       ' '.join('%08x' % int(y) for y in w[max(0, x - 4):x + 5]))


def section_skip(addr, w, undef, fidx, ranges=(), data=(), code=()):
    """the skip map of one section: sorted uint64 addresses. w = stock words at addr, fidx = function start indexes,
    ranges = [(start, end)] data-in-code addresses, data / code = reviewed addresses"""
    n = len(w)
    c, dd = rules_cd(w, undecodable(w, undef), np.arange(n, dtype=np.int64), fidx)
    flag = (c | dd) & ~is_pac(w)
    for lo, hi in ranges:                                  # LC_DATA_IN_CODE: data, whatever the bits say
        a, b = max(lo - addr, 0) // 4, min(-(-(hi - addr) // 4), n)
        if a < b:
            flag[a:b] = True
    for lst, val in ((code, False), (data, True)):         # the reviewer has the last word
        lst = lst if isinstance(lst, np.ndarray) else np.array(sorted(lst), dtype=np.uint64)
        lo, hi = np.searchsorted(lst, np.uint64(addr)), np.searchsorted(lst, np.uint64(addr + 4 * n))
        for va in lst[lo:hi]:
            flag[(int(va) - addr) // 4] = val
    return np.uint64(addr) + np.uint64(4) * np.flatnonzero(flag).astype(np.uint64)


def build_map(stock, undef, data=(), code=(), progress=None):
    """-> (sorted uint64 array of addresses, {image path: count})"""
    S = Cache(stock)
    data, code = np.array(sorted(data), dtype=np.uint64), np.array(sorted(code), dtype=np.uint64)
    out, per = [], {}
    for path, base, lc, ncmds in S.images():
        sects, linkedit, fst, dic = load_commands(lc, ncmds)
        fsa = function_starts(S, base, linkedit, fst)
        ranges = data_in_code(S, base, linkedit, dic)
        for sn, addr, size in sects:
            sv = S.view(addr, size & ~3)
            if sv is None or len(sv) < 4:
                continue
            a = section_skip(addr, np.frombuffer(sv, '<u4'), undef, (fsa - addr) // 4, ranges, data, code)
            if len(a):
                out.append(a)
                per[path] = per.get(path, 0) + len(a)
    arr = np.sort(np.concatenate(out)) if out else np.zeros(0, np.uint64)
    return arr, per


def verify(stock, new, skip):
    """every word of NEW whose address is in the skip map must equal the stock word -> list of (path, address, stock, new)"""
    S, R = Cache(stock), Cache(new)
    bad, seen = [], 0
    for path, base, lc, ncmds in S.images():
        sects, _, _, _ = load_commands(lc, ncmds)
        for sn, addr, size in sects:
            lo, hi = np.searchsorted(skip, np.uint64(addr)), np.searchsorted(skip, np.uint64(addr + (size & ~3)))
            if lo == hi:
                continue
            sv, rv = S.view(addr, size & ~3), R.view(addr, size & ~3)
            if sv is None or rv is None or len(sv) != len(rv):
                bad.append((path, addr, 0, 0))
                continue
            idx = ((skip[lo:hi] - np.uint64(addr)) // np.uint64(4)).astype(np.int64)
            w, r = np.frombuffer(sv, '<u4')[idx], np.frombuffer(rv, '<u4')[idx]
            seen += len(idx)
            for i in np.flatnonzero(w != r):
                bad.append((path, addr + 4 * int(idx[i]), int(w[i]), int(r[i])))
    return bad, seen


def read_list(path):
    data, code = [], set()
    for ln in open(path):
        p = ln.split('#')[0].split()
        if len(p) >= 2 and p[1] == 'code':
            code.add(int(p[0], 16))
        elif len(p) >= 3:
            data.append((int(p[0], 16), int(p[1], 16), int(p[2], 16)))
    return data, code


def apply(cachedir, lst, dry=False, revert=False):
    maps = []
    for p in sorted(glob.glob(cachedir + '/dyld_shared_cache_arm64e*')):
        if p.endswith(('.map', '.atlas', '.symbols')):
            continue
        with open(p, 'rb') as f:
            h = f.read(0x200)
            if not h.startswith(b'dyld_v1'):
                continue
            mo, mc = struct.unpack_from('<II', h, 0x10)
            f.seek(mo)
            for _ in range(mc):
                a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
                maps.append((a, s, o, p))
    ents, _ = read_list(lst)
    plan, already, bad = [], 0, []
    for va, damaged, stock in ents:
        hit = [(o + va - a, p) for a, s, o, p in maps if a <= va < a + s]
        if not hit:
            bad.append('%#x: address not mapped' % va)
            continue
        off, p = hit[0]
        with open(p, 'rb') as f:
            f.seek(off)
            cur = struct.unpack('<I', f.read(4))[0]
        want, other = (damaged, stock) if revert else (stock, damaged)
        if cur == want:
            already += 1
        elif cur == other:
            plan.append((p, off, want))
        else:
            bad.append('%#x: holds %08x, neither %08x nor %08x' % (va, cur, damaged, stock))
    if bad:
        print('textdata-guard: NOT APPLIED (another cache build?):\n' + '\n'.join('  ' + b for b in bad[:20]))
        return 1
    if not dry:
        for p, off, want in plan:
            with open(p, 'r+b') as f:
                f.seek(off)
                f.write(struct.pack('<I', want))
    print('textdata-guard: %d words %s, %d already there, %d listed%s' % (
        len(plan), 'damaged again' if revert else 'restored', already, len(ents), ' (dry run)' if dry else ''))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['scan', 'apply', 'check', 'map', 'verify'])
    ap.add_argument('a')
    ap.add_argument('b', nargs='?')
    ap.add_argument('--undef')
    ap.add_argument('--reviewed')
    ap.add_argument('--out', help='map: the .npy file to write')
    ap.add_argument('--map', help='verify: the .npy skip map')
    ap.add_argument('--pac', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--revert', action='store_true')
    o = ap.parse_args()
    if o.mode == 'apply':
        sys.exit(apply(o.a, o.b, o.dry_run, o.revert))
    if o.mode == 'verify':
        if not (o.b and o.map):
            ap.error('verify needs STOCK_MAIN NEW_MAIN --map skip.npy')
        skip = np.load(o.map)
        bad, seen = verify(o.a, o.b, skip)
        for path, va, sw, nw in bad[:50]:
            print('textdata-guard: %s %#x stock %08x new %08x' % (path, va, sw, nw), file=sys.stderr)
        print('textdata-guard: verify: %d of %d mapped words checked differ from the stock cache' % (len(bad), seen),
              file=sys.stderr)
        sys.exit(1 if bad else 0)
    if not o.undef:
        ap.error('--undef is required')
    undef = np.array(sorted(int(x, 16) for x in open(o.undef).read().split()), dtype=np.uint32)
    if o.mode == 'map':
        if not o.out:
            ap.error('map needs --out skip.npy')
        data, code = read_list(o.reviewed) if o.reviewed else ([], set())
        arr, per = build_map(o.a, undef, [va for va, _, _ in data], code)
        np.save(o.out, arr)
        print('textdata-guard: skip map: %d words in %d images (%d reviewed as data, %d as code) -> %s (%d bytes)' % (
            len(arr), len(per), len(data), len(code), o.out, os.path.getsize(o.out)))
        return
    if not o.b:
        ap.error('%s needs STOCK_MAIN NEW_MAIN' % o.mode)
    known = set()
    if o.mode == 'check':
        data, code = read_list(o.reviewed) if o.reviewed else ([], set())
        known = {va for va, _, _ in data} | code
    new = 0
    for path, sn, va, sw, rw, rules, ctx in scan(o.a, o.b, undef, o.pac):
        if o.mode == 'check' and va in known:
            continue
        new += 1
        print('%s\t%s\t%#x\t%08x\t%08x\t%s\t%s' % (path, sn, va, sw, rw, rules, ctx))
    if o.mode == 'check':
        print('textdata-guard: %d candidate(s) not in the reviewed list' % new, file=sys.stderr)
        sys.exit(1 if new else 0)


if __name__ == '__main__':
    main()
