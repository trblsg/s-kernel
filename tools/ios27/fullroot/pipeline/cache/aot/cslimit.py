import sys,glob,os,struct
def be(b,o,n): return int.from_bytes(b[o:o+n],'big')
bad=0;n=0;eq=0
for p in sorted(glob.glob(sys.argv[1]+'*')):
    if p.endswith(('.map','.atlas','.a2s')) or os.path.isdir(p): continue
    d=open(p,'rb').read(0x4000)
    if not d.startswith(b'dyld_v1'): continue
    mo,mc=struct.unpack_from('<II',d,0x10); cso,css=struct.unpack_from('<QQ',d,0x28)
    maps=[struct.unpack_from('<QQQII',d,mo+32*i) for i in range(mc)]
    mx=max(o+s for a,s,o,_,_ in maps)
    f=open(p,'rb'); f.seek(cso); b=f.read(css)
    off=be(b,16,4); cl=be(b,off+32,4)
    n+=1; eq+= (cl==mx)
    if cl<mx: bad+=1; print('NOT COVERED',os.path.basename(p),hex(cl),hex(mx))
print('files',n,'codeLimit==maxmapend',eq,'uncovered',bad)
