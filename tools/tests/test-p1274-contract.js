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
has(lab,'p1274_ticks_ms(200)','p1274_ticks_ms(225000)','p1274_ticks_ms(230000)','p1274_ticks_ms(240000)','p1274_ticks_ms(420000)','p1274_ticks_ms(250)','p1274_ticks_ms(10000)','p1274_ticks_ms(5000)','hakios_exynos_tmu_zone_now_mc(0)','p1274_active_profile','p1274_conflict','MIN(stock,cap)','p1274_writers','hakios_p1273_trace_stop','p1274_requested_mask','if(v>=P1274_REC2 && ocp_on)return EPERM','CTLFLAG_ANYBODY','if(!pid)return EPERM');
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
console.log('P1274 source contracts, lower-only/mask/QH models PASS (not native compile or hardware)');
