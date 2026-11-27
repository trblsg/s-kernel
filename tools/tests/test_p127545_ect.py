"""Synthetic ECT fixture; no firmware or device access."""
import importlib.util
from pathlib import Path
import struct

path = Path(__file__).resolve().parents[1] / "p127545-ect.py"
spec = importlib.util.spec_from_file_location("ect", path)
ect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ect)

def u32(v):
    return struct.pack("<I", v)

def entry(name, offset):
    b = name.encode("ascii")
    return u32(len(b)) + (b+b"\0").ljust((len(b)+4)&~3, b"\0") + u32(offset)

def fixture():
    d = bytearray(256)
    d[:16] = b"PARA" + b"0001" + u32(256) + u32(1)
    d[16:32] = entry("MINLOCK", 32)
    m = memoryview(d)[32:]
    m[:12] = u32(1)+b"0001"+u32(2)
    m[12:32] = entry("dvfs_mif",56)
    m[32:52] = entry("dvfs_int",80)
    m[56:76] = struct.pack("<5I",2,1794000,400000,421000,107000)
    m[80:92] = struct.pack("<3I",1,400000,200000)
    return bytes(d)

def reject(data):
    try:
        ect.parse(data)
    except ect.Invalid:
        return
    raise AssertionError("accepted invalid ECT")

d = fixture()
r = ect.parse(d)
assert r["mif_to_int"] == [{"master_khz":1794000,"min_slave_khz":400000},{"master_khz":421000,"min_slave_khz":107000}]
assert ect.find(b"\0"*16+d)["offset"]==16
assert ect.evaluate(r,1794000,200000,[400000])[0]["table_constraints_satisfied"]
assert not ect.evaluate(r,1794000,73000,[400000])[0]["table_constraints_satisfied"]
assert not ect.evaluate(r,1794000,336000,[107000])[0]["table_constraints_satisfied"]
for n in range(len(d)):
    reject(d[:n])
for off,val in [(28,16),(32+56,257),(32+68,1794000),(32+84,0)]:
    bad=bytearray(d)
    struct.pack_into("<I",bad,off,val)
    reject(bad)
try:
    ect.find(d+d)
except ect.Invalid:
    pass
else:
    raise AssertionError("accepted ambiguous image")
print("ECT Python synthetic fixture/truncation/dependency tests PASS")
