'use strict';
// Source and bounded state models. Not a native kernel compile or device proof.
const fs=require('fs'),path=require('path'),a=require('assert');
const read=f=>fs.readFileSync(path.join(__dirname,'../..',f),'utf8');
const sys=read('drivers/power/exynos/hakios.p125_sysctl.h'),cpu=read('drivers/power/exynos/hakios.exynos_cpufreq.c'),
  x=read('xnu/files/hakios_p125_impl.h'),mali=read('drivers/gpu/mali/hakios.mali.c'),
  dpu=read('drivers/display/dpu/hakios.exynos_dpu.c'),host=read('tools/s9-p125-timer-serial.ps1'),
  serial=read('tools/s9-p12-dual-serial.ps1'),metal=read('userland/src/metalvk/NativeBridgeMetalFrontend.mm');
const has=(s,...v)=>v.forEach(k=>a(s.includes(k),k));
has(x,'if(old!=0)return false','hakios_p1273_identity','th->thread_id','task->task_uniqueid',
    'if(on)hakios_p1273_trace_stop(4,mach_absolute_time())','now+duration<end');
has(mali,'hakios_p1273_identity(&mali_queue[at].trace_tid','tr.generation=mali_queue[mali_queue_head].trace_generation',
    'mali_submit_observed(chain,fragment,tr.generation?&wake:NULL)','p1273_gpu_first_wake=tr',
    'ended<=__atomic_load_n(&hakios_p1273_trace_until','obs->attempted=!g3d_awake');
const gpage=mali.slice(mali.indexOf('p1273_gpu_page_sysctl('),mali.indexOf('SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gpu_page'));
a(!/\b(?:rd|wr|g3d_ensure_on)\s*\(/.test(gpage),'GPU trace readback must not access hardware');
has(sys,'p1273_gov_record(now)','P1273_GOV_SET(big_mc','seq!=__atomic_load_n(&r->seq',
    'p1273_cap_guard','250ULL*NSEC_PER_MSEC','15ULL*NSEC_PER_SEC','60ULL*NSEC_PER_SEC',
    'return MIN(stock,target)','now>=(word&~3ULL)','!hakios_p123_input_quiet(now)',
    'if(v==2 && !p1273_cap_mild_complete)','if(v!=0 && v!=60)return EINVAL',
    'span<fixed+setup','p125_finish(10)','p125_finish(8)','hakios_p127_stop(reason)',
    'extras && (extras&(extras-1))');
has(cpu,'max=p1273_cap_ceiling(c,max)','p1273_cap_tick(now)');
has(dpu,'static unsigned psr_owner','lck_spin_lock(&psr_lock)','psr_shadow(void)',
    'if(want && psr_b && psr_drained)','psr_mask_locked(2,psr_ms?psr_ms:100)',
    'if(psr_b){\n        psr_drained=true;','rearm=psr_drained && psr_ms && psr_call');
a(!dpu.includes('|| mode!=2 || psr_ms)return'));
has(metal,'HAKIOS_METAL_LOG_AB','HAKIOS_METAL_LAYOUT_PLAN','native-metal-p1273-desc','p1273LayoutPlan(d,1,backed)');
has(host,'ThermalGpuDump','OffBurstCapABA','OfflineABA','P1273-OfflineIntegrate',
    'P1273 planned offline freeze/rollback not proven','P1273 cap phase revoked');
has(serial,'p1273_offline_s','p1273_gpu_page','p1273_cap_mode');
// Lower-only cap, expired/unsafe path returns stock.
const cap=(stock,request,now,until,safe)=>safe&&now<until&&request?Math.min(stock,request):stock;
for(const stock of [598000,1053000,1469000,1794000])for(const request of [0,1053000,1261000,1469000]){
    a(cap(stock,request,1,15,true)<=stock);
    a.equal(cap(stock,request,15,15,true),stock);
    a.equal(cap(stock,request,1,15,false),stock);
}
// Fixed window never extends policy lease; early link or policy revoke invalid.
const end=(down,duration,probe,policy)=>Math.min(down+duration,probe,policy);
a.equal(end(20,60,120,120),80);a.equal(end(80,60,120,120),120);
const reason=(deadline,up,now,policyRevoked)=>up&&up<deadline?8:policyRevoked?6:now>=deadline?10:0;
a.equal(reason(80,70,90,false),8);a.equal(reason(80,90,90,false),10);
a.equal(reason(80,0,79,false),0);a.equal(reason(80,0,90,true),6);
// PSR ownership model: only owned masks are released; B waits for legacy drain.
let owner=1,b=false,drained=false;
const enter=()=>{owner=0;b=true;drained=false;};
const poll=()=>{if(b&&drained&&!owner)owner=2;};
enter();poll();a.equal(owner,0);drained=true;poll();a.equal(owner,2);
owner=0;b=false;drained=false;poll();a.equal(owner,0);
const gate=read('drivers/power/exynos/hakios.p1273_gate.h');
has(gate,'case 903:return P1273_G_C5','case 919:return P1273_G_C6','(qualified&required)==required','e->bt_post_rx','e->wifi_handled','e->floor_restored');
has(sys,'p1273_gate_stop_ok_locked()','p1273_gate_missing_locked()','p1273_gate_qualified=0','p1273_cap_measured_ok>=2','R(P1273_FLOOR_STOCK)>0');
has(cpu,'p1273_measured_sample(mach_absolute_time())');
has(host,'P1273-FunctionalGate','P1273-CombinedPermission','P1273 foreground wake not observed','P1273 no qualification','GATE-FOREGROUND');
has(serial,'P1273GateAllowed','P1273CombinedAllowed');
has(read('drivers/map'),'power/exynos bsd/dev/hakios.p1273_gate.h');
has(read('userland/src/metal_test.m'),'p1273-metal-log-aba','p1273-layout-check','p1273-log-child','p1273-layout-child');
a(!read('userland/rc.jobs').includes('cputop 10'));
for(let q=0;q<256;q++){
    const allowed=(m,enabled)=>enabled&&(m===903?(q&31)===31:m===919?(q&127)===127:false);
    a(!allowed(903,false));a(!allowed(919,false));a(!allowed(1023,true));
    if(allowed(919,true))a(allowed(903,true));
}
console.log('P12.7.3 source contracts and cap/offline/PSR/functional gate state models PASS (not native compile)');
