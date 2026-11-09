import struct,sys
def be(b,o,n): return int.from_bytes(b[o:o+n],'big')
def dump(path):
    d=open(path,'rb').read(); cso,css=struct.unpack_from('<QQ',d,0x28)
    b=d[cso:cso+css]
    print(path.split('/')[-1],'file',hex(len(d)),'cs',hex(cso),hex(css),'blob magic',hex(be(b,0,4)),'len',hex(be(b,4,4)),'count',be(b,8,4))
    for i in range(be(b,8,4)):
        t,o=be(b,12+8*i,4),be(b,16+8*i,4)
        m=be(b,o,4); L=be(b,o+4,4)
        print('  idx type',hex(t),'off',hex(o),'magic',hex(m),'len',hex(L))
        if m==0xfade0c02:
            v,fl,ho,io,nsp,ncs,cl,hs,ht,pl,ps=struct.unpack('>IIIIIIIBBBB',b[o+8:o+8+32+0]) if False else (be(b,o+8,4),be(b,o+12,4),be(b,o+16,4),be(b,o+20,4),be(b,o+24,4),be(b,o+28,4),be(b,o+32,4),b[o+36],b[o+37],b[o+38],b[o+39])
            print('    CD ver',hex(v),'flags',hex(fl),'hashOff',hex(ho),'identOff',hex(io),'nSpecial',nsp,'nCode',ncs,'codeLimit',hex(cl),'hashSize',hs,'hashType',ht,'pageSizeLog2',ps)
for p in sys.argv[1:]: dump(p)
