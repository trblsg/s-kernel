#!/usr/bin/env node
'use strict';
// Read-only audit of the actual iOS ARM64 ICD. No dylib execution, no SDK
// or Apple tools required. Stream byte sizes and chained imports are NOT
// counts of executed binds; export count is not a latency measurement.
const fs=require('fs'),crypto=require('crypto');
function audit(b){
 const need=(o,n)=>{if(!Number.isSafeInteger(o)||!Number.isSafeInteger(n)||o<0||n<0||o>b.length||n>b.length-o)throw Error('Mach-O range outside file: '+o+'+'+n);};
 const u32=o=>{need(o,4);return b.readUInt32LE(o);};
 const u64=o=>{need(o,8);const n=Number(b.readBigUInt64LE(o));if(!Number.isSafeInteger(n))throw Error('64-bit size/offset exceeds safe range');return n;};
 const name=(o,n)=>{need(o,n);return b.subarray(o,o+n).toString('utf8').replace(/\0.*$/s,'');};
 const cstr=(o,end)=>{need(o,end-o);const z=b.indexOf(0,o);if(z<0||z>=end)throw Error('unterminated string');return b.subarray(o,z).toString('utf8');};
 need(0,32);if(u32(0)!==0xfeedfacf||u32(4)!==0x100000c||u32(12)!==6)throw Error('expected thin little-endian arm64 Mach-O dylib');
 const ncmd=u32(16),cmdbytes=u32(20);need(32,cmdbytes);
 const r={schema:1,sha256:crypto.createHash('sha256').update(b).digest('hex'),file_bytes:b.length,cpu_subtype:u32(8),flags:'0x'+u32(24).toString(16),uuid:null,dependencies:[],install_name:null,segments:[],initializers:[],dyld_info:null,chained_fixups:null,export_trie:null,symbol_table:null,build_versions:[],limitations:['Static structure, not runtime attribution.','Classic bind/rebase stream sizes are bytes, not executed fixup counts.','Chained imports and populated chain pages are not pointer-fixup counts.','Initializer section entries are potential initializers, not measured constructor calls.','Stripped symbol names and compressed chained symbol pools may be unavailable.']};
 let sym=null,trie=null,chain=null;
 for(let i=0,o=32;i<ncmd;i++){
  need(o,8);const cmd=u32(o),sz=u32(o+4),end=o+sz;
  if(sz<8||end>32+cmdbytes||sz%8)throw Error('invalid load command');
  const min=n=>{if(sz<n)throw Error('short load command '+cmd.toString(16));};
  if(cmd===0x19){
   min(72);const seg={name:name(o+8,16),vm_bytes:u64(o+32),file_offset:u64(o+40),file_bytes:u64(o+48),sections:[]};need(seg.file_offset,seg.file_bytes);
   const ns=u32(o+64);if(ns>Math.floor((sz-72)/80))throw Error('section count outside command');
   for(let j=0;j<ns;j++){
    const s=o+72+j*80,flags=u32(s+64),type=flags&255;
    const sec={name:name(s,16),size:u64(s+40),offset:u32(s+48),type};
    seg.sections.push(sec);
    if(type===9||type===0x16||sec.name==='__mod_init_func'||sec.name==='__init_offsets'){
     const width=type===0x16||sec.name==='__init_offsets'?4:8;
     if(sec.size%width)throw Error('initializer section size mismatch');need(sec.offset,sec.size);
     const values=[];for(let k=0;k<sec.size;k+=width)values.push('0x'+(width===4?u32(sec.offset+k).toString(16):b.readBigUInt64LE(sec.offset+k).toString(16)));
     r.initializers.push({segment:seg.name,section:sec.name,entries:sec.size/width,raw_values:values});
    }
   }r.segments.push(seg);
  }else if(cmd===0x1b){min(24);r.uuid=b.subarray(o+8,o+24).toString('hex');}
  else if([0xc,0xd,0x80000018,0x8000001f,0x20,0x80000023].includes(cmd)){
   min(24);const off=u32(o+8);if(off<24||off>=sz)throw Error('bad dylib name offset');const d=cstr(o+off,end);
   if(cmd===0xd)r.install_name=d;else r.dependencies.push({name:d,command:'0x'+cmd.toString(16)});
  }else if(cmd===2){min(24);sym={off:u32(o+8),count:u32(o+12),str:u32(o+16),bytes:u32(o+20)};}
  else if(cmd===0xb){min(80);r.dynamic_symbol_table={locals:u32(o+12),external_defined:u32(o+20),undefined:u32(o+28),external_relocations:u32(o+68),local_relocations:u32(o+76)};}
  else if(cmd===0x22||cmd===0x80000022){
   min(48);const x={};for(const [j,k] of ['rebase','bind','weak_bind','lazy_bind','export'].entries()){
    const off=u32(o+8+j*8),len=u32(o+12+j*8);need(off,len);x[k+'_bytes']=len;if(k==='export'&&len&&!trie)trie={off,len};
   }r.dyld_info=x;
  }else if(cmd===0x80000033||cmd===0x80000034){min(16);const x={off:u32(o+8),len:u32(o+12)};need(x.off,x.len);if(cmd===0x80000033)trie=x;else chain=x;}
  else if(cmd===0x32){min(24);r.build_versions.push({platform:u32(o+8),minimum_os:u32(o+12),sdk:u32(o+16)});}
  o=end;if(i===ncmd-1&&o!==32+cmdbytes)throw Error('load command size mismatch');
 }
 if(sym){
  need(sym.off,sym.count*16);need(sym.str,sym.bytes);
  const exported=[],weak_dynamic=[],dynamic=[],undef=[];
  for(let i=0;i<sym.count;i++){
   const o=sym.off+i*16,typ=b[o+4],desc=b.readUInt16LE(o+6),idx=u32(o);
   if((typ&0xe0)||!(typ&1))continue;
   const n=idx<sym.bytes?cstr(sym.str+idx,sym.str+sym.bytes):null;
   if(n===null)throw Error('symbol string index out of range');
   if((typ&14)===0){
    undef.push(n);if((desc>>>8)===0xfe){dynamic.push(n);if(desc&0x40)weak_dynamic.push(n);}
   }else if(!(typ&0x10))exported.push(n);
  }
  r.symbol_table={nlist_count:sym.count,defined_external_count:exported.length,undefined_external_count:undef.length,dynamic_lookup_count:dynamic.length,weak_dynamic_lookup_count:weak_dynamic.length,defined_external_names:exported.sort(),undefined_external_names:undef.sort(),dynamic_lookup_names:dynamic.sort(),weak_dynamic_lookup_names:weak_dynamic.sort()};
 }
 if(trie&&trie.len){
  const base=trie.off,end=base+trie.len,names=[];
  function leb(pos){let value=0,m=1;for(let k=0;k<10;k++){if(pos>=end)throw Error('truncated ULEB');const x=b[pos++];value+=(x&127)*m;if(!Number.isSafeInteger(value))throw Error('ULEB overflow');if(!(x&128))return [value,pos];m*=128;}throw Error('ULEB overflow');}
  function visit(off,prefix,anc){
   if(off>=trie.len||anc.has(off)||anc.size>512||names.length>1000000)throw Error('invalid export trie');
   let [term,pos]=leb(base+off);if(term>end-pos)throw Error('bad export terminal');if(term)names.push(prefix);pos+=term;if(pos>=end)throw Error('missing trie children');
   const count=b[pos++],next=new Set(anc);next.add(off);
   for(let i=0;i<count;i++){const edge=cstr(pos,end);pos+=Buffer.byteLength(edge)+1;let child;[child,pos]=leb(pos);visit(child,prefix+edge,next);}
  }visit(0,'',new Set());r.export_trie={bytes:trie.len,count:names.length,names:names.sort()};
 }
 if(chain&&chain.len){
  const o=chain.off,end=o+chain.len;if(chain.len<28)throw Error('short chained header');
  const version=u32(o),starts=u32(o+4),imports=u32(o+8),symbols=u32(o+12),count=u32(o+16),format=u32(o+20),symbols_format=u32(o+24);
  if(version!==0||starts>=chain.len||imports>chain.len||symbols>chain.len)throw Error('unsupported chained header');
  const width=({1:4,2:8,3:16})[format]||(!count?1:0);if(!width||count>Math.floor((chain.len-imports)/width))throw Error('invalid chained import array');
  const list=[];for(let i=0;i<count;i++){
   const q=o+imports+i*width;let lib,weak,off;
   if(format===3){const v=b.readBigUInt64LE(q);lib=Number(BigInt.asIntN(16,v&65535n));weak=!!(v&65536n);off=Number(v>>32n);}
   else{const v=u32(q);lib=(v&255)<<24>>24;weak=!!(v&256);off=v>>>9;}
   const text=symbols_format===0?cstr(o+symbols+off,end):null;
   list.push({ordinal:lib,weak,name:text});
  }
  const st=o+starts;if(st+4>end)throw Error('short starts table');const ns=u32(st);if(ns>Math.floor((end-st-4)/4))throw Error('bad chain segment count');const pages=[];
  for(let i=0;i<ns;i++){
   const rel=u32(st+4+4*i);if(!rel)continue;const q=st+rel;if(q+22>end)throw Error('bad chain segment start');const size=u32(q),page_size=b.readUInt16LE(q+4),pointer_format=b.readUInt16LE(q+6),page_count=b.readUInt16LE(q+20);
   if(size<22+2*page_count||q+size>end)throw Error('bad chain page table');
   let populated=0,multi=0;for(let j=0;j<page_count;j++){const x=b.readUInt16LE(q+22+j*2);if(x!==65535){populated++;if(x&32768)multi++;}}
   pages.push({segment_index:i,page_size,pointer_format,page_count,populated_pages:populated,multi_start_pages:multi});
  }
  r.chained_fixups={bytes:chain.len,imports_count:count,imports_format:format,symbols_format,weak_imports:list.filter(x=>x.weak).length,flat_lookup_imports:list.filter(x=>x.ordinal===-2).length,weak_flat_lookup_imports:list.filter(x=>x.ordinal===-2&&x.weak).length,pages,imports:list};
 }
 r.initializer_entries=r.initializers.reduce((n,x)=>n+x.entries,0);
 return r;
}
module.exports={audit};
if(require.main===module){
 if(process.argv.length!==4){console.error('usage: node p1275-macho-audit.js INPUT.dylib REPORT.json');process.exit(2);}
 try{const r=audit(fs.readFileSync(process.argv[2]));fs.writeFileSync(process.argv[3],JSON.stringify(r,null,2)+'\n');console.log(JSON.stringify({sha256:r.sha256,uuid:r.uuid,bytes:r.file_bytes,exports:r.export_trie?.count??r.symbol_table?.defined_external_count??null,weak_dynamic:r.symbol_table?.weak_dynamic_lookup_count??null,chained_imports:r.chained_fixups?.imports_count??null,initializers:r.initializer_entries}));}catch(e){console.error(e.message);process.exit(1);}
}
