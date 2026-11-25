'use strict';
const assert=require('assert'),{makeMap,parse}=require('../p125-symbol-map.js');
// Synthetic, minimal ARM64 linked images. Explicit __TEXT_EXEC section and
// local nlist symbols model the real XNU linker layout, not nm output.
const base=0xfffffff007100000n;
const required=['_hakios_p125_active','_thread_timer_expire','_thread_call_delayed_timer','_mk_timer_expire'];
function image({strip=false,change=false,uuid=7,missing=false,duplicate=false,legacy=false}={}){
 const symbols=[
  ...required.map((name,i)=>({name,off:i*4,type:i===0?15:14,sec:1})),
  {name:'_unrelated_data',off:0,type:15,sec:2},
  {name:'_debug_stab',off:4,type:0x2e,sec:1},
  {name:'_outside_code',off:128,type:15,sec:1}
 ].filter(r=>!(strip&&r.type===14)&&!(missing&&r.name===required[0]));
 if(duplicate)symbols.push({name:required[0],off:20,type:15,sec:1});
 const cmdBytes=152*2+24*2,codeOff=32+cmdBytes,dataOff=codeOff+64,symOff=dataOff+16,strOff=symOff+16*symbols.length;
 const strings=Buffer.from('\0'+symbols.map(r=>r.name+'\0').join(''));
 const b=Buffer.alloc(strOff+strings.length);
 const u=(o,v)=>b.writeUInt32LE(v,o),q=(o,v)=>b.writeBigUInt64LE(v,o);
 u(0,0xfeedfacf);u(4,0x0100000c);u(12,2);u(16,4);u(20,cmdBytes);
 function segment(o,seg,sec,addr,len,off,prot,flags){
  u(o,0x19);u(o+4,152);b.write(seg,o+8);q(o+24,addr);q(o+32,BigInt(len));q(o+40,BigInt(off));q(o+48,BigInt(len));u(o+56,prot);u(o+60,prot);u(o+64,1);
  const s=o+72;b.write(sec,s);b.write(seg,s+16);q(s+32,addr);q(s+40,BigInt(len));u(s+48,off);u(s+64,flags);
 }
 segment(32,legacy?'__TEXT':'__TEXT_EXEC','__text',base,64,codeOff,5,0x80000400);
 segment(184,'__DATA','__data',base+4096n,16,dataOff,3,0);
 u(336,0x1b);u(340,24);b.fill(uuid,344,360);
 u(360,2);u(364,24);u(368,symOff);u(372,symbols.length);u(376,strOff);u(380,strings.length);
 b.fill(change?0xbb:0xaa,codeOff,dataOff);
 let idx=1;
 symbols.forEach((r,i)=>{const o=symOff+i*16;u(o,idx);b[o+4]=r.type;b[o+5]=r.sec;q(o+8,(r.sec===2?base+4096n:base)+BigInt(r.off));idx+=Buffer.byteLength(r.name)+1;});
 strings.copy(b,strOff);
 return b;
}
const full=image(),stripped=image({strip:true});
const map=makeMap(stripped,full);
assert.strictEqual(map.count,4);
assert.strictEqual(map.anchors.hakios_p125_active,base.toString(16));
assert(map.text.includes(' T _hakios_p125_active'));
assert(map.text.includes(' t _thread_timer_expire'));
assert(!map.text.includes('_unrelated_data'));
assert(!map.text.includes('_debug_stab'));
assert(!map.text.includes('_outside_code'));
assert.strictEqual(makeMap(image({legacy:true}),null).count,4);
assert.throws(()=>makeMap(stripped,null),/missing or ambiguous/);
assert.throws(()=>makeMap(stripped,image({change:true})),/sections do not match/);
assert.throws(()=>makeMap(stripped,image({uuid:8})),/UUID mismatch/);
assert.throws(()=>makeMap(image({missing:true}),null),/missing or ambiguous/);
assert.throws(()=>makeMap(image({duplicate:true}),null),/missing or ambiguous/);
assert.throws(()=>parse(full.subarray(0,40),'truncated'),/invalid file extent/);
console.log('P12.5 Mach-O symbol map PASS: __TEXT_EXEC, __TEXT, local symbols, stripped pairing, data/STAB filtering, wrong image, missing/duplicate anchors, malformed input');
