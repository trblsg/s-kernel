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
console.log('P125 pinned exact source '+total+' anchors PASS');
