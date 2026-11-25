'use strict';
const fs=require('fs'), path=require('path'), os=require('os'), assert=require('assert'), cp=require('child_process');
const read=f=>fs.readFileSync(path.join(__dirname,'../..',f),'utf8');
const mali=read('drivers/gpu/mali/hakios.mali.c'),lab=read('drivers/gpu/mali/hakios.mali_lab.h'),
    client=read('drivers/gpu/mali/hakios.mali.cpp'),test=read('userland/src/mali_test.c');
function fn(s,name){
    const re=new RegExp('^(?:static )?(?:bool|void|int|uint32_t)\\s*\\n'+name+'\\([^]*?\\)\\s*\\n\\{','m');
    const m=re.exec(s);assert(m,name+' definition missing');
    const end=s.indexOf('\n}',m.index+m[0].length);assert(end>=0,name+' end missing');return s.slice(m.index,end+2);
}
const cap=fn(mali,'hakios_mali_parameter'),reader=fn(mali,'p1272_read_cap_on'),
    capture=fn(mali,'p1272_capture_caps'),ready=fn(mali,'hakios_mali_ready'),
    serve=fn(mali,'step_serve'),power=fn(mali,'p1272_power_present'),guard=fn(mali,'p1272_mmio_ok'),
    irq=fn(mali,'mali_job_interrupt'),submit=fn(mali,'hakios_mali_submit');
for(const f of [cap,ready,irq])assert(!/\b(?:rd|wr|wait_for|g3d_power_on|g3d_ensure_on|as0_update)\s*\(/.test(f),f);
assert(!cap.includes('lck_mtx_'));
assert(ready.includes('__ATOMIC_ACQUIRE'));
assert(cap.includes('which>=P1272_CAPS') && cap.includes('p1272_cap_mask') && cap.includes('p1272_caps[which]'));
assert(serve.indexOf('p1272_capture_caps()')<serve.indexOf('&p1272_cap_ready,1,__ATOMIC_RELEASE') &&
    serve.indexOf('&p1272_cap_ready,1,__ATOMIC_RELEASE')<serve.indexOf('HakiosMaliStart()'));
assert.equal((mali.match(/p1272_read_cap_on\(/g)||[]).length,2,'boot-only reader must not escape');
assert(capture.includes('valid!=P1272_CAP_MASK') && capture.includes('p1272_mmio_ok()'));
assert(power.includes('PMU_G3D_STATUS') && power.includes('PLL_ENABLE|PLL_STABLE') && power.includes('MUX_G3D_USER'));
assert(guard.includes('g3d_awake && g3d_lab_gpu_safe && g3d_lab_protection_ok'));
assert(fn(mali,'g3d_ensure_on').includes('p1272_require_mmio()'));
assert(fn(mali,'hakios_mali_unmap').includes('p1272_mmio_ok()') && fn(mali,'hakios_mali_mapped').includes('p1272_mmio_ok()'));
assert(submit.indexOf('g3d_ensure_on()')<submit.indexOf('wr(mali,'));
assert(submit.includes('if(!gpu_reset() || !gpu_cores_on(&which))'));
assert(fn(mali,'g3d_idle_gate').indexOf('mali_irq_off();')<fn(mali,'g3d_idle_gate').indexOf('g3d_off();'));
assert(fn(lab,'p1272_probe_request_sysctl').includes('p1272_probe_used') &&
    fn(lab,'p1272_probe_request_sysctl').includes('clock_interval_to_deadline(60,NSEC_PER_SEC'));
assert(client.includes('size!=sizeof(r.param)') && client.includes('hakios_mali_parameter(r.param.param'));
assert(test.includes('IOConnectCallStructMethod(gpu,kGetParam') && test.includes('OFF_GET_PARAM PASS count=41') &&
    test.includes('get_all_caps(on_caps) && !memcmp(off_caps,on_caps,sizeof(off_caps))'));
assert(read('userland/rc.jobs').includes('mali_test --p1272-auto &'));
assert(read('tools/s9-p125-timer-serial.ps1').includes('GpuCacheCheck'));
console.log('P12.7.2 source contract: immutable 41-param cache, publication, OFF MMIO/IRQ and userclient qualification PASS');
if(process.argv.includes('--compile')){
    const defs=mali.split(/\r?\n/).filter(l=>/^#define (P1272_CAPS|P1272_CAP_MASK|PMU_G3D_STATUS|G3D_ON|PLL_CON0|PLL_ENABLE|PLL_STABLE|MUX_G3D_USER|MUX_SEL|THREAD_TLS_ALLOC|THREAD_MAX_THREADS|SHADER_PRESENT)\s/.test(l)).join('\n');
    const harness=read('tools/tests/p1272-cache-fixture.c.in').replace('/* DEFS */',defs).replace('/* FUNCTIONS */',[power,guard,ready,reader,capture,cap].join('\n'));
    const dir=fs.mkdtempSync(path.join(os.tmpdir(),'p1272-'));
    try{
        const f=path.join(dir,'test.c'),exe=path.join(dir,process.platform==='win32'?'test.exe':'test');
        fs.writeFileSync(f,harness);
        cp.execFileSync(process.env.CC||'cc',['-std=c11','-Wall','-Wextra','-Werror','-O2',f,'-o',exe],{stdio:'inherit'});
        cp.execFileSync(exe,[],{stdio:'inherit'});
    }finally{fs.rmSync(dir,{recursive:true,force:true});}
}
