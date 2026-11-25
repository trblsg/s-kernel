const fs=require('fs'),path=require('path');
const root=process.argv[2]||'xnu-src/fork-271ae546513d';
const src=fs.readFileSync('xnu/p125-timer-observer.py','utf8');
const edits=JSON.parse(src.match(/EDITS=json.loads\(r'''([\s\S]*?)'''\)/)[1]);
let total=0;
for(const [file,arr] of Object.entries(edits)){
 let s=fs.readFileSync(path.join(root,file),'utf8').replace(/\r\n/g,'\n');
 if(file==='osfmk/arm/rtclock.c'){
   const old='ml_set_decrementer((uint32_t) delay_time);\n\n\treturn delay_time;';
   const neu='ml_set_decrementer((uint32_t) delay_time);\n\thakios_p1241_pop_observe((unsigned)cdp->cpu_number,time,current_time,(unsigned)delay_time); /* P1241_POP_OBSERVER */\n\n\treturn delay_time;';
   if(s.split(old).length!==2)throw Error('P1241 anchor');
   s=s.replace(old,neu);
 }
 let out=s;
 for(const [old,neu] of arr){
   if(out.split(old).length!==2)throw Error(file+' anchor '+JSON.stringify(old.slice(0,120))+' count '+(out.split(old).length-1));
   out=out.replace(old,neu);total++;
 }
 if(out.split('/* P125_OBSERVER */').length!==2)throw Error(file+' marker');
 for(const [old,neu] of arr)if(out.split(neu).length!==2)throw Error(file+' idempotence '+old.slice(0,80));
 for(const [old,neu] of [...arr].reverse())out=out.replace(neu,old);
 if(out!==s)throw Error(file+' inverse');
 console.log('PASS '+file+' '+arr.length+' anchors');
}
const makefile=fs.readFileSync(path.join(root,'osfmk/kern/Makefile'),'utf8').replace(/\r\n/g,'\n');
const anchor='XNU_ONLY_EXPORTS = '+String.fromCharCode(92)+'\n';
const entry='\thakios_p125.h '+String.fromCharCode(92)+'\n';
if(makefile.split(anchor).length!==2 || makefile.includes('hakios_p125.h'))throw Error('export anchor');
const patched=makefile.replace(anchor,anchor+entry);
if(patched.split(anchor+entry).length!==2 || patched.replace(anchor+entry,anchor)!==makefile)throw Error('export round-trip');
const exportList=patched.split(anchor)[1].split('\n\n')[0];
if(!exportList.includes(entry) || !patched.includes('${XNU_ONLY_EXPORTS}'))throw Error('header not in export list');
const mapping=fs.readFileSync('drivers/map','utf8').split(/\r?\n/);
if(mapping.filter(l=>l==='power/exynos bsd/dev/hakios.p125_sysctl.h').length!==1)throw Error('missing driver map entry');
for(const [f,target] of [['xnu/files/hakios_p125.h','osfmk/kern/hakios_p125.h'],['xnu/files/hakios_p125_impl.h','osfmk/arm64/hakios_p125_impl.h']]){
 if(!fs.existsSync(f)||!fs.readFileSync('profiles/ios-16.5.copy','utf8').split(/\r?\n/).includes(f+' '+target))throw Error('missing overlay '+f);
}
console.log('P125 pinned exact source '+total+' C/C++ anchors + XNU-only export + header maps PASS');
