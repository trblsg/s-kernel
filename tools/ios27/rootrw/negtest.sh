#!/bin/bash
# negative tests: the checker must FAIL on tampered staged files
cd "$(dirname "$0")"
PY=${PY:-python3}
T=/usr/libexec/safetycheckd           # T mode
A=/usr/sbin/mDNSResponder             # A mode
rm -rf neg && mkdir -p neg
mk() { rm -rf neg/s; mkdir -p neg/s$(dirname $1); cp -p stage$1 neg/s$1; }
run() { $PY macho-check.py --stage neg/s --only <(echo $1) 2>&1 | egrep "^FAIL|FAILS" | head -3; }
python3 - <<'PYEOF'
import struct,shutil,os
def flip(path, off, mask=0xff):
    b=bytearray(open(path,'rb').read()); b[off]^=mask; open(path,'wb').write(b)
PYEOF
echo "== 1 control (untampered T)"; mk $T; run $T
echo "== 2 T: corrupt a byte of a body"; mk $T
$PY - "$T" <<'PYEOF'
import sys,struct,subprocess
p='neg/s'+sys.argv[1]; b=bytearray(open(p,'rb').read())
# find first site B and its thunk's BL body: simplest: flip a byte in the stub region = last nonzero 64 bytes of TEXT padding: locate the thunk word STR x30
import re
i=b.find(struct.pack('<I',0xF81700FE)) if False else -1
pat=struct.pack('<I',0xF8000C00 | ((-144)&0x1ff)<<12 | 31<<5 | 30)
i=b.find(pat); assert i>0
# body follows somewhere before thunks; corrupt the word right after the thunk's 4 words
j=i-8; b[j]^=0x40; open(p,'wb').write(b)
PYEOF
run $T
echo "== 3 T: restore one site to the original (site left)"; mk $T
$PY - "$T" <<'PYEOF'
import sys
p='neg/s'+sys.argv[1]; r='ref'+sys.argv[1]
a=bytearray(open(p,'rb').read()); b=open(r,'rb').read()
d=[i for i in range(0,len(b)-3,4) if a[i:i+4]!=b[i:i+4] and i>0x4000]
a[d[0]:d[0]+4]=b[d[0]:d[0]+4]; open(p,'wb').write(a)
PYEOF
run $T
echo "== 4 T: change an unrelated byte (data)"; mk $T
$PY - "$T" <<'PYEOF'
import sys
p='neg/s'+sys.argv[1]; a=bytearray(open(p,'rb').read()); a[len(a)-5000]^=1; open(p,'wb').write(a)
PYEOF
run $T
echo "== 5 A: flip a byte of the code signature"; mk $A
$PY - "$A" <<'PYEOF'
import sys,struct
p='neg/s'+sys.argv[1]; a=bytearray(open(p,'rb').read())
o=32
n=struct.unpack_from('<I',a,16)[0]
for _ in range(n):
    c,s=struct.unpack_from('<II',a,o)
    if c==0x1d: off,sz=struct.unpack_from('<II',a,o+8); a[off+100]^=1; break
    o+=s
open(p,'wb').write(a)
PYEOF
run $A
echo "== 6 A: shrink __STUBX vmsize field"; mk $A
$PY - "$A" <<'PYEOF'
import sys,struct
p='neg/s'+sys.argv[1]; a=bytearray(open(p,'rb').read())
o=32
n=struct.unpack_from('<I',a,16)[0]
for _ in range(n):
    c,s=struct.unpack_from('<II',a,o)
    if c==0x19 and a[o+8:o+15]==b'__STUBX': struct.pack_into('<Q',a,o+32,0x1000); break
    o+=s
open(p,'wb').write(a)
PYEOF
run $A
echo "== 7 A: break chained fixups seg_count"; mk $A
$PY - "$A" <<'PYEOF'
import sys,struct
p='neg/s'+sys.argv[1]; a=bytearray(open(p,'rb').read())
o=32
n=struct.unpack_from('<I',a,16)[0]
for _ in range(n):
    c,s=struct.unpack_from('<II',a,o)
    if c==0x80000034:
        fo,fs=struct.unpack_from('<II',a,o+8); so=struct.unpack_from('<I',a,fo+4)[0]; struct.pack_into('<I',a,fo+so,struct.unpack_from('<I',a,fo+so)[0]-1); break
    o+=s
open(p,'wb').write(a)
PYEOF
run $A
