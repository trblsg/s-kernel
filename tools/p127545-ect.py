#!/usr/bin/env python3
"""Read Samsung Exynos ECT PARA/MINLOCK, never write hardware.
Format: Apollo 30656faf0758 drivers/soc/samsung/ect_parser.c
(ect_parse_binary_header, ect_parse_string, ect_parse_minlock_header).
Input must be a raw ECT blob or an extracted firmware image, not a ZIP/TAR.
A table is source evidence, NOT proof that this firmware runs on the device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

MAX_ECT = 64 * 1024 * 1024
MAX_IMAGE = 512 * 1024 * 1024
MAX_ENTRIES = 256

class Invalid(ValueError):
    pass

def u32(data, pos, end):
    if pos < 0 or pos + 4 > end:
        raise Invalid("truncated integer")
    return struct.unpack_from("<I", data, pos)[0], pos + 4

def name(data, pos, end):
    n, pos = u32(data, pos, end)
    stop = pos + ((n + 1 + 3) & ~3)
    if not 1 <= n <= 128 or stop > end or data[pos+n] != 0:
        raise Invalid("invalid ECT name")
    try:
        s = bytes(data[pos:pos+n]).decode("ascii")
    except UnicodeError as exc:
        raise Invalid("non-ASCII ECT name") from exc
    if not all(c.isalnum() or c in "_.-" for c in s):
        raise Invalid("invalid ECT name characters")
    return s, stop

def directory(data, pos, end, count, base):
    if not 1 <= count <= MAX_ENTRIES:
        raise Invalid("invalid directory count")
    entries = {}
    for _ in range(count):
        s, pos = name(data, pos, end)
        off, pos = u32(data, pos, end)
        if s in entries or off & 3 or off >= end-base:
            raise Invalid("duplicate name or invalid offset")
        entries[s] = base+off
    if len(set(entries.values())) != len(entries) or min(entries.values()) < pos:
        raise Invalid("overlapping directory")
    ordered = sorted(entries.values()) + [end]
    return {k:(v, ordered[ordered.index(v)+1]) for k,v in entries.items()}

def parse(data, base=0):
    if base < 0 or base+16 > len(data) or data[base:base+4] != b"PARA":
        raise Invalid("missing PARA header")
    size, count = struct.unpack_from("<II", data, base+8)
    if not 16 <= size <= MAX_ECT or base+size > len(data):
        raise Invalid("invalid ECT total size")
    end = base+size
    blocks = directory(data, base+16, end, count, base)
    if "MINLOCK" not in blocks:
        raise Invalid("MINLOCK not present")
    start, stop = blocks["MINLOCK"]
    version, pos = u32(data, start, stop)
    if version > 16 or pos+8 > stop:
        raise Invalid("unsupported MINLOCK header")
    label = bytes(data[pos:pos+4]).hex()
    n, pos = u32(data, pos+4, stop)
    dom = directory(data, pos, stop, n, start)
    result = {}
    for key in ("dvfs_mif", "dvfs_int"):
        if key not in dom:
            raise Invalid("required domain absent: "+key)
        a,b = dom[key]
        levels, a = u32(data,a,b)
        if not 1 <= levels <= MAX_ENTRIES or a+8*levels > b:
            raise Invalid("invalid level count: "+key)
        rows = [struct.unpack_from("<II",data,a+8*i) for i in range(levels)]
        # Exact lookup only, no interpolation. Zero slave frequency is
        # retained in raw evidence but never accepted as an admissible floor.
        if len({x for x,y in rows}) != levels or any(
            not 1000 <= x <= 4000000 or y > 4000000 or (y != 0 and y < 1000)
            for x,y in rows):
            raise Invalid("invalid/duplicate frequency pair: "+key)
        result[key] = [{"master_khz":x,"min_slave_khz":y} for x,y in rows]
    return {
        "offset":base,"size":size,"ect_sha256":hashlib.sha256(data[base:end]).hexdigest(),
        "ect_version_hex":bytes(data[base+4:base+8]).hex(),
        "minlock_parser_version":version,"minlock_version_hex":label,
        "mif_to_int":result["dvfs_mif"],"int_to_fsys0":result["dvfs_int"],
    }

def find(data, offset=None):
    if offset is not None:
        return parse(data,offset)
    valid = []
    pos = 0
    while True:
        at = data.find(b"PARA",pos)
        if at < 0:
            break
        pos = at+4
        if at & 3:
            continue
        try:
            valid.append(parse(data,at))
        except Invalid:
            continue
        if len(valid)>1:
            raise Invalid("multiple valid ECT blobs; specify --offset")
    if not valid:
        raise Invalid("no valid ECT MINLOCK blob found")
    return valid[0]

def lookup(rows, master):
    found=[r["min_slave_khz"] for r in rows if r["master_khz"]==master]
    return found[0] if len(found)==1 and found[0]>0 else None

def evaluate(report, mif, fsys0, candidates):
    floor=lookup(report["mif_to_int"],mif)
    out=[]
    for target in candidates:
        need=lookup(report["int_to_fsys0"],target)
        ok=floor is not None and need is not None and target>=floor and fsys0>=need
        out.append({"int_khz":target,"mif_min_int_khz":floor,"int_min_fsys0_khz":need,
                    "table_constraints_satisfied":ok})
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image",type=Path)
    ap.add_argument("--offset",type=lambda s:int(s,0))
    ap.add_argument("--mif-khz",type=int)
    ap.add_argument("--fsys0-khz",type=int)
    ap.add_argument("--int-khz",type=int,nargs="+",default=[534000,400000,336000,267000,178000,107000])
    args=ap.parse_args()
    try:
        if args.image.stat().st_size>MAX_IMAGE:
            raise Invalid("extract a raw ECT or firmware image <=512 MiB first")
        data=args.image.read_bytes()
        report=find(data,args.offset)
        report.update({"source":args.image.name,"source_sha256":hashlib.sha256(data).hexdigest(),
                       "device_firmware_identity_verified":False,"live_rates_verified":False,
                       "write_allowed":False})
        if (args.mif_khz is None)!=(args.fsys0_khz is None):
            raise Invalid("provide both MIF and FSYS0 or neither")
        if args.mif_khz is not None:
            if not 1000<=args.mif_khz<=4000000 or not 1000<=args.fsys0_khz<=4000000 or any(not 1000<=x<=4000000 for x in args.int_khz):
                raise Invalid("frequency outside bounds")
            report["assumed_rates"]={"mif_khz":args.mif_khz,"fsys0_khz":args.fsys0_khz}
            report["candidate_review"]=evaluate(report,args.mif_khz,args.fsys0_khz,args.int_khz)
        print(json.dumps(report,indent=2))
    except (OSError,Invalid) as exc:
        print("ECT refused: "+str(exc),file=sys.stderr)
        return 2
    return 0

if __name__=="__main__":
    sys.exit(main())
