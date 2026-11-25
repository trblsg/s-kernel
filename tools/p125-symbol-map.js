/* P12.5: read the linked ARM64 Mach-O nlist table, independently of nm's
 * __TEXT vs __TEXT_EXEC classification. Export code symbols only.
 * Pair the unstripped image with the actual installer kernel before use. */
'use strict';
const fs=require('fs'), crypto=require('crypto');
const need=(ok,msg)=>{if(!ok)throw Error(msg)};
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
function parse(b,label){
  const bounds=(off,n)=>need(Number.isSafeInteger(off)&&Number.isSafeInteger(n)&&off>=0&&n>=0&&off<=b.length&&n<=b.length-off,label+': invalid file extent');
  const u32=o=>{bounds(o,4);return b.readUInt32LE(o)};
  const u64=o=>{bounds(o,8);return b.readBigUInt64LE(o)};
  const str=(o,n)=>{bounds(o,n);return b.subarray(o,o+n).toString('utf8').split('\0')[0]};
  bounds(0,32);
  need(u32(0)===0xfeedfacf && u32(4)===0x0100000c && u32(12)===2,label+': expected thin ARM64 MH_EXECUTE');
  const ncmd=u32(16),end=32+u32(20); bounds(32,end-32);
  let o=32, sections=[null], sym=null, uuid=null, exec=[];
  for(let i=0;i<ncmd;i++){
    need(o+8<=end,label+': truncated load command');
    const cmd=u32(o),size=u32(o+4); need(size>=8&&size%8===0&&o+size<=end,label+': bad load command size');
    if(cmd===0x19){
      need(size>=72,label+': short segment');
      const seg=str(o+8,16), prot=u32(o+60), nsec=u32(o+64);
      need(nsec<=Math.floor((size-72)/80),label+': bad section count');
      for(let j=0;j<nsec;j++){
        const q=o+72+j*80,name=str(q,16),sseg=str(q+16,16),addr=u64(q+32),len=u64(q+40),off=u32(q+48),flags=u32(q+64);
        need(seg===sseg,label+': section segment mismatch');
        const code=!!(prot&4) && ((flags&0x80000000)!==0 || (flags&0x400)!==0 || (name==='__text'&&(seg==='__TEXT'||seg==='__TEXT_EXEC')));
        sections.push({seg,name,addr,len,code});
        if(code&&len){
          need(len<=BigInt(Number.MAX_SAFE_INTEGER),label+': section too large'); bounds(off,Number(len));
          exec.push([seg,name,addr.toString(16),len.toString(16),hash(b.subarray(off,off+Number(len)))]);
        }
      }
    }else if(cmd===2){
      need(size>=24&&!sym,label+': invalid LC_SYMTAB');sym=[u32(o+8),u32(o+12),u32(o+16),u32(o+20)];
    }else if(cmd===0x1b){
      need(size===24&&!uuid,label+': invalid LC_UUID');uuid=b.subarray(o+8,o+24).toString('hex');
    }
    o+=size;
  }
  need(o===end&&exec.length,label+': no executable sections / command mismatch');
  const rows=[];
  if(sym){
    const [off,count,strings,sz]=sym; bounds(off,count*16); bounds(strings,sz);
    for(let i=0;i<count;i++){
      const q=off+i*16,idx=u32(q),type=b[q+4],sec=sections[b[q+5]],value=u64(q+8);
      if((type&0xe0)||(type&0x0e)!==0x0e||!sec||!sec.code||value<sec.addr||value>=sec.addr+sec.len||!idx||idx>=sz)continue;
      const pos=strings+idx,z=b.indexOf(0,pos);
      need(z>=pos&&z<strings+sz,label+': unterminated symbol');
      const name=b.toString('utf8',pos,z);
      if(!name||/[\s\x00-\x1f\x7f]/.test(name))continue;
      rows.push({value,name,type:(type&1)?'T':'t'});
    }
  }
  return {rows,uuid,exec:exec.sort((a,b)=>a[0].localeCompare(b[0])||a[1].localeCompare(b[1]))};
}
const required=['hakios_p125_active','thread_timer_expire','thread_call_delayed_timer','mk_timer_expire'];
function makeMap(shipped,full){
  const a=parse(shipped,'installer kernel'), b=full?parse(full,'unstripped kernel'):a;
  if(full){
    need(JSON.stringify(a.exec)===JSON.stringify(b.exec),'unstripped executable sections do not match installer kernel');
    need(a.uuid===b.uuid,'unstripped LC_UUID mismatch');
  }
  const seen=new Map();
  for(const r of b.rows){
    const key=r.value.toString(16)+' '+r.name;
    if(!seen.has(key)||r.type==='T')seen.set(key,r);
  }
  const rows=[...seen.values()];
  const anchors={};
  for(const name of required){
    const matches=rows.filter(r=>r.name===name||r.name==='_'+name);
    const addresses=new Set(matches.map(r=>r.value.toString(16)));
    need(addresses.size===1,'missing or ambiguous code symbol '+name+' (code symbols='+rows.length+')');
    const address=[...addresses][0];anchors[name]=address;
    const shippedMatches=a.rows.filter(r=>r.name===name||r.name==='_'+name);
    need(shippedMatches.every(r=>r.value.toString(16)===address),'installer symbol mismatch '+name);
  }
  // Normalize duplicate spellings of required anchors so host's strict parser
  // sees exactly one row. Prefer original underscore spelling if present.
  const unique=rows.filter(r=>!required.some(n=>r.name===n&&rows.some(s=>s.name==='_'+n&&s.value===r.value)));
  unique.sort((x,y)=>x.value<y.value?-1:x.value>y.value?1:x.name.localeCompare(y.name));
  return {text:unique.map(r=>r.value.toString(16).padStart(16,'0')+' '+r.type+' '+r.name).join('\n')+'\n',
    count:unique.length,anchors,uuid:a.uuid,executable_sections:a.exec,shipped_sha256:hash(shipped),symbol_source_sha256:hash(full||shipped)};
}
if(require.main===module){
  try{
    const [shipped,unstripped,out]=process.argv.slice(2);
    need(shipped&&unstripped&&out,'usage: node p125-symbol-map.js KERNEL UNSTRIPPED_OR_DASH OUTPUT');
    const full=unstripped==='-'?null:fs.readFileSync(unstripped);
    const r=makeMap(fs.readFileSync(shipped),full);
    fs.writeFileSync(out,r.text);
    const {text,...metadata}=r;
    fs.writeFileSync(out+'.json',JSON.stringify(metadata,null,2)+'\n');
    console.log('P12.5 Mach-O code symbol map PASS: '+r.count+' symbols; anchor='+r.anchors.hakios_p125_active+' source='+unstripped);
  }catch(e){console.error('P12.5 symbol map: '+e.message);process.exitCode=1;}
}
module.exports={parse,makeMap};
