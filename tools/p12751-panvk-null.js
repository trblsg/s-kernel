#!/usr/bin/env node
'use strict';
// P12751: only remove final-link unresolved weak entries in generated
// Mesa dispatch tables. Never patch binary bytes, function bodies or headers.
const fs=require('fs'),path=require('path'),assert=require('assert/strict'),crypto=require('crypto');
const {audit}=require('./p1275-macho-audit.js');
const allowed=/^_(?:panvk(?:_v(?:6|7|9|10))?|vk(?:_common)?|wsi)_[A-Za-z][A-Za-z0-9_]*$/;
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const read=p=>fs.readFileSync(p,'utf8');
function once(s,from,to){assert.equal(s.split(from).length,2,'generator/link anchor missing or ambiguous: '+from);return s.replace(from,to);}
function files(root){
 const out=new Map();
 function walk(dir){for(const e of fs.readdirSync(dir,{withFileTypes:true})){
  const f=path.join(dir,e.name);if(e.isDirectory()){walk(f);continue;}
  if(!e.isFile()||!/entrypoints.*\.(?:c|cc|cpp)$/.test(e.name))continue;
  const s=read(f);if(!s.includes('This file generated from vk_entrypoints_gen.py'))continue;
  out.set(path.relative(root,f).split(path.sep).join('/'),s);
 }}walk(root);assert(out.size,'no generated entrypoint tables');return out;
}
function rewrite(s,missing,hits){
 let table=false;
 const lines=s.split(/(?<=\n)/);
 return lines.map(line=>{
  if(/^const struct vk_(?:instance|physical_device|device)_entrypoint_table \w+ = \{\s*$/.test(line)){assert(!table,'nested table');table=true;return line;}
  if(table&&/^};\s*$/.test(line)){table=false;return line;}
  if(!table)return line;
  const m=/^([ \t]*\.[A-Za-z_]\w* = )([A-Za-z_]\w*)(,[ \t]*\r?\n?)$/.exec(line);
  if(!m||!missing.has(m[2]))return line;
  hits.set(m[2],(hits.get(m[2])||0)+1);
  return m[1]+'0'+m[3]; // C null pointer constant; same as unresolved weak.
 }).join('');
}
const sdkWeak=['_pthread_mutexattr_destroy','_pthread_mutexattr_init','_pthread_mutexattr_settype'];
function sdkWeakRecords(a){
 const rows=a.symbol_table.weak_undefined_records.filter(x=>x.ordinal!==0xfe);
 for(const x of rows){
  assert(sdkWeak.includes(x.name),'unknown SDK weak import: '+x.name);
  assert(x.ordinal>=1 && a.dependencies[x.ordinal-1]?.name==='/usr/lib/libSystem.B.dylib','SDK weak import not bound to libSystem: '+x.name);
 }
 return rows;
}
function verifyWeak(a,b){
 assert.equal(b.symbol_table.dynamic_lookup_count,0,'optimized dynamic lookup remains');
 assert.equal(b.symbol_table.weak_dynamic_lookup_count,0,'optimized weak dynamic lookup remains');
 assert.deepEqual(sdkWeakRecords(b),sdkWeakRecords(a),'SDK weak import ABI differs');
}
function missingFrom(a){
 const s=a.symbol_table;assert(s&&a.export_trie,'stock nlist/export trie missing');
 const names=s.weak_dynamic_lookup_names;
 assert(names.length>0 && s.dynamic_lookup_count===names.length,'stock missing nonweak or empty dynamic list');
 assert(names.every(n=>allowed.test(n)),'unexpected dynamic weak name outside generated namespaces');
 assert.equal(new Set(names).size,names.length,'duplicate stock weak symbol');
 assert(!names.some(n=>a.export_trie.names.includes(n)),'missing symbol overlaps export');
 assert(a.dyld_info&&!a.chained_fixups,'unexpected stock bind format');
 return new Set(names.map(n=>n.slice(1)));
}
function plan(stock,root){
 const missing=missingFrom(stock),hits=new Map(),expected=new Map();
 for(const [f,s] of files(root))expected.set(f,rewrite(s,missing,hits));
 assert.equal(hits.size,missing.size,'not all final-link weak imports have generated dispatch provenance; refuse');
 for(const n of missing)assert(hits.has(n),'missing slot: '+n);
 return {missing,hits,expected};
}
function prepare(stockFile,stockBuild,optSource,manifest){
 const stock=audit(fs.readFileSync(stockFile)),p=plan(stock,stockBuild);
 sdkWeakRecords(stock);
 const gen=path.join(optSource,'src/vulkan/util/vk_entrypoints_gen.py');
 let s=read(gen);
 s=once(s,'    .${e.name} = ${p}_${e.name},',"    .${e.name} = ${'0' if gen_weak and (p + '_' + e.name) in p12751_missing else p + '_' + e.name},");
 s=once(s,"        'gen_weak': args.gen_weak,","        'gen_weak': args.gen_weak,\n        'p12751_missing': frozenset("+JSON.stringify([...p.missing].sort())+"),");
 fs.writeFileSync(gen,s);
 const meson=path.join(optSource,'src/panfrost/vulkan/meson.build');
 fs.writeFileSync(meson,once(read(meson),"panvk_darwin_link_args = ['-Wl,-undefined,dynamic_lookup']","panvk_darwin_link_args = ['-Wl,-undefined,error']"));
 const result={schema:1,stock_sha256:stock.sha256,stock_uuid:stock.uuid,missing_count:p.missing.size,slots:[...p.hits].sort((a,b)=>a[0].localeCompare(b[0])),generated_files:[...p.expected].map(([name,s])=>({name,expected_sha256:hash(s)}))};
 fs.writeFileSync(manifest,JSON.stringify(result,null,2)+'\n');console.log('P12751 proven generated missing symbols '+p.missing.size);
}
function verify(stockFile,optFile,stockBuild,optBuild,manifest,out){
 const a=audit(fs.readFileSync(stockFile)),b=audit(fs.readFileSync(optFile)),p=plan(a,stockBuild),actual=files(optBuild),m=JSON.parse(read(manifest));
 assert.equal(m.stock_sha256,a.sha256);assert.equal(m.missing_count,p.missing.size);
 assert.deepEqual([...actual.keys()].sort(),[...p.expected.keys()].sort(),'generated table set differs');
 for(const [f,s] of p.expected)assert.equal(actual.get(f),s,'unexpected generated table change: '+f);
 verifyWeak(a,b);
 assert(b.dyld_info&&!b.chained_fixups,'unexpected optimized bind format; audit classic binds first');
 assert.equal(b.initializer_entries,a.initializer_entries,'initializer count changed');
 assert.deepEqual(b.export_trie.names,a.export_trie.names,'export ABI differs');
 assert.deepEqual(b.dependencies,a.dependencies,'dependency list differs');
 const unresolved=a.symbol_table.undefined_external_names.filter(n=>!a.symbol_table.weak_dynamic_lookup_names.includes(n));
 assert.deepEqual(b.symbol_table.undefined_external_names,unresolved,'non-target undefined imports changed');
 assert(b.uuid&&a.uuid&&b.uuid!==a.uuid,'variant UUID not distinct');
 assert.equal(b.install_name,'/redwood/lib/libvulkan_panfrost_opt.dylib');
 fs.writeFileSync(out,JSON.stringify({schema:1,stock_sha256:a.sha256,opt_sha256:b.sha256,stock_uuid:a.uuid,opt_uuid:b.uuid,removed:p.missing.size,stock_exports:a.export_trie.count,opt_exports:b.export_trie.count,stock_dynamic:a.symbol_table.dynamic_lookup_count,opt_dynamic:b.symbol_table.dynamic_lookup_count,sdk_weak_preserved:sdkWeakRecords(b),dispatch_source_equivalence:true,strict_link_required:true,native_functional_verified:false},null,2)+'\n');
 console.log('P12751 strict link + generated dispatch equivalence PASS; removed '+p.missing.size+' weak dynamic imports');
}
function verifyPack(stockFile,optFile,report){
 const a=audit(fs.readFileSync(stockFile)),b=audit(fs.readFileSync(optFile)),r=JSON.parse(read(report));
 assert.equal(r.schema,1);assert.equal(r.stock_sha256,a.sha256);assert.equal(r.opt_sha256,b.sha256);
 assert.equal(r.stock_uuid,a.uuid);assert.equal(r.opt_uuid,b.uuid);
 assert.equal(r.dispatch_source_equivalence,true);assert.equal(r.strict_link_required,true);
 assert.equal(r.removed,missingFrom(a).size);
 verifyWeak(a,b);
 assert.deepEqual(b.export_trie.names,a.export_trie.names);assert.deepEqual(b.dependencies,a.dependencies);
 assert.equal(b.install_name,'/redwood/lib/libvulkan_panfrost_opt.dylib');assert(a.uuid!==b.uuid);
 console.log('P12751 verified cached stock/opt identities and symbol gates');
}
module.exports={rewrite,missingFrom,plan,once,prepare,verify,verifyPack,sdkWeakRecords,verifyWeak};
if(require.main===module){try{
 const [cmd,...args]=process.argv.slice(2);
 if(cmd==='prepare'&&args.length===4)prepare(...args);
 else if(cmd==='verify'&&args.length===6)verify(...args);
 else if(cmd==='verify-pack'&&args.length===3)verifyPack(...args);
 else throw Error('prepare STOCK STOCK_BUILD OPT_SOURCE MANIFEST | verify STOCK OPT STOCK_BUILD OPT_BUILD MANIFEST REPORT | verify-pack STOCK OPT REPORT');
}catch(e){console.error(e.stack);process.exitCode=1;}}
