"""Verify exact decoder PPS and window tables against the pinned Samsung source.
Run locally with the retrieved reference directory; no downloads or device I/O.
"""
from pathlib import Path
import re, sys, hashlib
root=Path(__file__).resolve().parents[2]
ref=Path(sys.argv[1])
def row(text, name):
    text=re.sub(r'/\*.*?\*/|//[^\n]*','',text,flags=re.S)
    m=re.search(r'\b'+name+r'\s*\[[^=]*?=\s*\{\s*\{([^}]*)\}',text,re.S)
    assert m, name
    return bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})\b',m[1]))
h=(root/'drivers/display/hakios.p1278_panel.h').read_text()
m=re.search(r'p1278_wqhd_pps\[128\]\s*=\s*\{([^}]*)\}',h,re.S)
pps=bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})\b',m[1])).ljust(128,b'\0')
assert len(pps)==128
for variant in ('s0','s1'):
    f=ref/f's6e3ha8_star_a3_{variant}_panel.h'
    data=f.read_bytes()
    s=data.decode('utf-8')
    pps_ref=row(s,f'star_a3_{variant}_pps_table')
    assert len(pps_ref)==128 and pps_ref==pps, (variant,len(pps_ref))
    assert row(s,f'star_a3_{variant}_scaler_table')==bytes.fromhex('01260808f3')
    assert row(s,f'star_a3_{variant}_caset_table')==bytes.fromhex('0000059f')
    assert row(s,f'star_a3_{variant}_paset_table')==bytes.fromhex('00000b8f')
    print(f'{variant}: exact 128-byte PPS/area/scaler match; source sha256={hashlib.sha256(data).hexdigest()}')
print('P1278 reference data PASS')
