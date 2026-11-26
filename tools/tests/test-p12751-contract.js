'use strict';
const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const r=p=>fs.readFileSync(path.join(__dirname,'../..',p),'utf8'),has=(s,...xs)=>xs.forEach(x=>assert(s.includes(x),x));
const lab=r('drivers/power/exynos/hakios.p1274_lab.h'),s=r('drivers/power/exynos/hakios.p12751.h'),h=r('tools/s9-p1274-serial.ps1'),host=r('tools/s9-p12-dual-serial.ps1'),build=r('tools/mk-panvk.sh'),gen=r('tools/p12751-panvk-null.js'),lazy=r('userland/src/metalvk/HakiosMetalVK.mm'),front=r('userland/src/metalvk/NativeBridgeMetalFrontend.mm'),init=r('userland/src/init.c'),cpu=r('userland/src/cputop.c'),impl=r('xnu/files/hakios_p125_impl.h');
// Check the actual source-copy manifest, not just presence in Git.
const root=path.join(__dirname,'../..');
const rows=r('drivers/map').split(/\r?\n/).map(l=>l.split('#')[0].trim()).filter(Boolean).map(l=>l.split(/\s+/));
const mapped=new Set(),destinations=new Set();
for(const [sub,dst,...extra] of rows){
 assert(dst && extra.length===0,'malformed drivers/map row');
 const src='drivers/'+sub+'/'+path.posix.basename(dst);
 assert(!mapped.has(src) && !destinations.has(dst),'duplicate drivers/map: '+src+' -> '+dst);
 assert(fs.statSync(path.join(root,src)).isFile(),'missing mapped file: '+src);
 mapped.add(src);destinations.add(dst);
}
function checkDriverTree(dir){
 for(const e of fs.readdirSync(path.join(root,dir),{withFileTypes:true})){
  const f=dir+'/'+e.name;
  if(e.isDirectory())checkDriverTree(f);
  else if(dir!=='drivers' && !['README.md','conf'].includes(e.name))
   assert(mapped.has(f),'unmapped driver (will not reach XNU): '+f);
 }
}
checkDriverTree('drivers');
assert(rows.some(([sub,dst])=>sub==='power/exynos' && dst==='bsd/dev/hakios.p12751.h'),'P12751 header must land beside the including file in bsd/dev');
assert(lab.includes('#include "hakios.p12751.h"'));
console.log('Driver copy manifest PASS: '+rows.length+' mapped files, P12751 in bsd/dev');
has(s,'pid!=1','expected=-1','p12751_stage_start_ok','p12751_stage_end_ok','p1274_ticks_ms(10000)','P12751_STAGES 512u','P12751_BOOT_STAGE_LIMIT 320u','r->at==v[2] && r->tid==tid && r->task==task','p12751_stage_sealed','p12751_next','p12751_ack');
has(build,'P12751_DUAL','libvulkan_panfrost_opt.dylib','p12751-panvk-null.js','flat-namespace','vk_smoke_opt');
has(gen,'gen_weak and','-Wl,-undefined,error','SDK weak import ABI differs','generated table set differs','export ABI differs','non-target undefined imports changed','verify-pack');
has(lazy,'S9P12751LayoutABI','MTLDeviceSPI','20F66','@encode(BOOL)','p12751_boot_mode','iosurfaceTextureAlignmentBytes');
has(front,'p12751_stage_event','_dyld_image_count','p12751Detailed','flags|16u');
has(init,'HakiosMetal.nextboot','p12751_boot_latch','var_kept');has(cpu,'p12751_ack','fsync','rename(','mediaserverd');
has(h,'P12751-CheckBoot','NextBoot','Read-OwnerFair','metal-stages-p12751.csv','p12751-stage-info.txt');
has(host,'P12751NextAllowed','p12751_stage_page','p12751_boot_latch');
has(impl,'hakios_p12751_trace_replace','dropped_ticks');
has(lab,'p12751_latched','hakios_p12751_trace_replace','p1274_mc>=76000 || p12741_lit_mc>=70000');
assert(!/\b(?:CPU_OFF|CPU_SUSPEND|processor_exit|reg_write|writel|psci_cpu_off)\s*\(/.test(s+lab));
console.log('P12751 source safety, boot latch, paired stages, dual build and host integration PASS (not native build)');
