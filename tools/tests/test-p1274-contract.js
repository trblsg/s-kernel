'use strict';
const fs=require('fs'),path=require('path'),assert=require('assert/strict');
const r=p=>fs.readFileSync(path.join(__dirname,'../..',p),'utf8');
const lab=r('drivers/power/exynos/hakios.p1274_lab.h'),cpu=r('drivers/power/exynos/hakios.exynos_cpufreq.c'),x=r('xnu/files/hakios_p125_impl.h'),idle=r('xnu/files/cpu.c'),metal=r('userland/src/metalvk/NativeBridgeMetalFrontend.mm'),host=r('tools/s9-p1274-serial.ps1');
const has=(s,...xs)=>xs.forEach(k=>assert(s.includes(k),k));
// Keep the eight-word event payload intact; the sysctl result must not
// redeclare any of the uint64_t payload parameters (native build regression).
const user=r('userland/src/p1274_user.h');
const emit=user.match(/static inline int p1274_emit\(([^)]*)\)\s*\{([\s\S]*?)\n\}/);
assert(emit,'missing p1274_emit');
const args=emit[1].split(',').map(x=>x.trim().split(/\s+/).pop());
const result=emit[2].match(/\bint\s+(\w+)\s*=\s*sysctlbyname\("kern\.hakios\.p1274_user_event"/);
assert(result && !args.includes(result[1]),'sysctl return variable collides with payload parameter');
has(emit[2],'uint64_t row[8]={kind,a,b,c,d,e,f,g};','return '+result[1]+';');
has(lab,'p1274_ticks_ms(200)','p1274_ticks_ms(225000)','p1274_ticks_ms(230000)','p1274_ticks_ms(240000)','p1274_ticks_ms(720000)','p1274_ticks_ms(250)','p1274_ticks_ms(10000)','p1274_ticks_ms(5000)','hakios_exynos_tmu_zone_now_mc(0)','p1274_active_profile','p1274_conflict','MIN(stock,cap)','p1274_writers','hakios_p1273_trace_stop','p1274_requested_mask','if(v>=P1274_REC2 && ocp_on)return EPERM','CTLFLAG_ANYBODY','if(!pid)return EPERM');
has(lab,'#define P1274_ROWS 768u','#define P1274_EVENTS 4096u','P12741_MINOR_LIMIT 24u','P12741_EPISODE_SAMPLES 40u','P12741_TIMELINE_SELECT 6000u','P12741_EVENT_SELECT 8192u','P12741_SAMPLE_SELECT 13000u','p12741_episode_tick(now,bits,severity,reason!=0)','if(!trigger && severity>=2)','p12741_guard_armed','p12741_lit_limit','p12741_profile,CTLTYPE_INT','p12741_revision','p12741_info','p12741_guard');
assert(!lab.includes('if(!profile && !old)return;'),'guard must survive lease');
assert(!lab.includes('OID_AUTO,p1274_profile,'),'old host write endpoint must not arm new lease');
assert(lab.indexOf('p1274_fast_tick(now,p);')<lab.indexOf('if(state!=2)return;',lab.indexOf('static void p1274_tick(')));
has(r('xnu/files/hakios_p125.h'),'P1274_FG_SLOTS 128u');
has(host,'P12741Deadline=720','p12741_revision','p12741_guard','episode-info.txt','raw_before_sync','net_battery_wh_250_710');
assert(host.indexOf('Range-N 5100')<host.indexOf("Read-OwnerBand $slots @('BIG','LITTLE')"));
assert(host.indexOf('Range-N 6000')<host.indexOf("Read-OwnerBand $slots @('BIG','LITTLE')"));
has(host,"Read-OwnerBand $near @('BIG')" ,'Read-ScalarRemainder','guard_peak_big_c');
has(user,'kern.hakios.p12741_user_state','if(!p1274_user_observing())return -1;');
has(lab,'p12741_user_state','p12741_guard_big_peak','p12741_guard_last_change');
has(cpu,'max=p1274_ceiling(c,max)','cores_set(p1274_cores(stock_cores,now), now)','b->measured = skip_big?0:measure_mhz(b)','p1274_tick(now)','p1274_dvfs_record');
has(x,'p1274_fg_record(ua,avg_ua,uv,external,charger,qh)','hakios_p1274_deadline','p1274_writer_enter','p1274_writer_leave');
assert(x.indexOf('p1274_fg_record(ua,avg_ua,uv,external,charger,qh)')<x.indexOf('if(!hakios_p127_mode() || !hakios_p125_active())return;',x.indexOf('void hakios_p127_fg_record')));
has(idle,'p1274_writer_enter(p12_began)','hakios_p1274_wfi_ticks');
has(metal,'p1274_user_mode()','mode!=2','p1274_emit(selector+1','nmb_device_open_observed');
has(r('tools/metal/bridge/NativeMetalObjects.rs'),'nmb_device_open_observed','f(7,1,0)','f(8,1,0)');
has(host,'P1274_BATCH_LIMIT','Check-Identity','Commit-Page','P125-Offline','Prior reviewed summary.json','[uint64]$s.usb_down','layout_calls_lower_bound','qh_raw_bracket_delta');
has(r('drivers/map'),'hakios.p1274_lab.h','hakios.p1274_policy.h');
assert(!/\b(?:CPU_OFF|CPU_SUSPEND|processor_exit|reg_write|writel|psci_cpu_off)\s*\(/.test(lab));
const cap=(stock,request,fast,active)=>Math.min(stock,fast||stock,active?(request||stock):stock);
for(const stock of [598000,1053000,1261000,1794000])for(const req of [0,1066000,1261000,1469000])for(const fast of [0,1066000,1261000]){
    assert(cap(stock,req,fast,true)<=stock);
    assert.equal(cap(stock,req,fast,false),Math.min(stock,fast||stock));
}
const mask=(ids,cores)=>ids.slice(cores).reduce((m,id)=>m&~(1<<id),255);
assert.deepEqual([4,2,1,0].map(n=>mask([3,4,5,6],n)),[255,159,143,135]);
const delta=(a,b)=>((b-a+98304)%65536)-32768;
assert.equal(delta(65530,2),8);assert.equal(delta(2,65530),-8);
const severity=(bits,mc,sum)=>bits&8||mc>=76000?3:bits&2||((bits&1)&&sum>=200&&mc>=67000)?2:bits?1:0;
assert.equal(severity(4,67000,60),1);
assert.equal(severity(1,67000,200),2);
assert.equal(severity(8,64000,0),3);
const ranges=[[512,639],[3072,4095],[4096,4223],[5000,5007],[5100,5131],[6000,6767],[8192,12287],[13000,14279]];
for(let i=1;i<ranges.length;i++)assert(ranges[i][0]>ranges[i-1][1],'selector overlap');
// A minor at 353s cannot take the major owner trace; the late event can.
let trigger=0; for(const [t,bits,mc,sum] of [[353,4,67000,60],[468,3,76000,380]])if(!trigger&&severity(bits,mc,sum)>=2)trigger=t;
assert.equal(trigger,468);
console.log('P1274.1 source contracts, selector ranges, late trigger and lower-only/mask/QH models PASS (not native compile or hardware)');
