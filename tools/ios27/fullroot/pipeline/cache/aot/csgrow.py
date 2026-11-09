"""csgrow.py - grow the code-signature blob of a dyld cache subcache file whose single TEXT mapping was grown by G bytes.
dyld (and the kernel's ubc_cs_is_range_codesigned) require the code signature to cover every mapped file range: in every
subcache of the stock cache CodeDirectory.codeLimit == the end of the last mapping. We append the SHA-256 page hashes of the
new range (16 KiB pages), raise nCodeSlots/codeLimit, and shift the blobs that follow the CodeDirectory."""
import hashlib, struct
PG = 0x4000
def be(b, o, n): return int.from_bytes(b[o:o + n], 'big')
def put(b, o, n, v): b[o:o + n] = int(v).to_bytes(n, 'big')

def parse(blob):
    assert be(blob, 0, 4) == 0xfade0cc0, 'not a SuperBlob'
    sb_len, cnt = be(blob, 4, 4), be(blob, 8, 4)
    ents = [(be(blob, 12 + 8 * i, 4), be(blob, 16 + 8 * i, 4)) for i in range(cnt)]
    cds = [(t, o) for t, o in ents if be(blob, o, 4) == 0xfade0c02]
    assert len(cds) == 1 and cds[0][0] == 0, ents
    cd_off = cds[0][1]
    cd_len = be(blob, cd_off + 4, 4)
    cd = dict(hashOff=be(blob, cd_off + 16, 4), nSpecial=be(blob, cd_off + 24, 4), nCode=be(blob, cd_off + 28, 4),
              codeLimit=be(blob, cd_off + 32, 4), hashSize=blob[cd_off + 36], hashType=blob[cd_off + 37], pageLog2=blob[cd_off + 39])
    return sb_len, ents, cd_off, cd_len, cd

def grow_blob(blob, s, G, stub, css):
    """blob: the old SuperBlob bytes (padded to css); s: old codeLimit (= old mapping end); G: growth; stub: the G new bytes."""
    assert G % PG == 0 and len(stub) == G
    sb_len, ents, cd_off, cd_len, cd = parse(blob)
    assert cd['hashSize'] == 32 and cd['hashType'] == 2 and cd['pageLog2'] == 14, cd
    assert cd['codeLimit'] == s and cd['nCode'] == s // PG, (cd, hex(s))
    assert cd['hashOff'] + cd['nCode'] * 32 == cd_len, 'code slots are not at the end of the CodeDirectory'
    add = G // PG
    slots = b''.join(hashlib.sha256(stub[i * PG:(i + 1) * PG]).digest() for i in range(add))
    delta = len(slots)
    new = bytearray(blob[:cd_off + cd_len]) + slots + bytearray(blob[cd_off + cd_len:sb_len])
    put(new, cd_off + 4, 4, cd_len + delta)
    put(new, cd_off + 28, 4, cd['nCode'] + add)
    put(new, cd_off + 32, 4, s + G)
    put(new, 4, 4, sb_len + delta)
    for i, (t, o) in enumerate(ents):
        if o > cd_off:
            put(new, 16 + 8 * i, 4, o + delta)
    assert len(new) == sb_len + delta
    out = bytes(new) + bytes(max(0, css - len(new)))
    newcss = len(out)
    if newcss > css:
        newcss = (newcss + PG - 1) & ~(PG - 1); out += bytes(newcss - len(out))
    return out
