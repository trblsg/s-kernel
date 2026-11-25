'use strict';
// Source contract regression, NOT a native XNU/hardware test.
const fs=require('fs'),assert=require('assert'),path=require('path');
const root=path.resolve(__dirname,'../..');
const get=f=>fs.readFileSync(path.join(root,f),'utf8').replace(/\r\n/g,'\n');
const h=get('xnu/files/hakios_p125.h'),impl=get('xnu/files/hakios_p125_impl.h');
const sys=get('drivers/power/exynos/hakios.p125_sysctl.h');
assert(h.includes('#define P126_MASK 63u'));
assert(sys.includes('120ULL*NSEC_PER_SEC'));
assert(sys.includes('__atomic_compare_exchange_n(&hakios_p126_lease'));
assert(impl.includes('if(on){hakios_p126_stop(4);hakios_p127_stop(4);}'));
assert(sys.includes('if(p125_thermal_reason())return EPERM'));
const hap=get('drivers/power/max77705/hakios.max77705.c');
assert(hap.indexOf('assert_wait_deadline((event_t)&heard_us')<hap.indexOf('uint64_t latest=__atomic_load_n(&heard_us'));
assert(hap.includes('thread_wakeup((event_t)&heard_us)'));
assert(hap.includes('!motor_on && !wanted && hakios_p126_active(P126_HAPTIC)'));
assert(hap.includes('else delay_for_interval(2, NSEC_PER_MSEC)'));
assert(hap.includes('HAPTIC_MAX_MS * 1000ull'));
const wifi=get('wifi/lx/src/lx_base.c');
assert(wifi.includes('lx_os_sleep(queue,lx_os_option("p126_worker_wait") ? 100000000ull : SLICE_NS)'));
assert(wifi.includes('lx_os_wakeup(queue)'));
assert(wifi.includes('lx_os_sleep(&jiffies, 2 * 1000000ull)'));
const ssp=get('drivers/input/ssp/hakios.ssp.c'),gnss=get('userland/src/gnss_chip.c');
for(const s of [ssp,gnss])assert(s.includes("GNSS_WAIT_READABLE _IOW('G',0x60,uint32_t)"));
assert(ssp.includes('if(!ms || ms>100)return EINVAL'));
assert(ssp.includes('msleep(&gnss_head,&shub_lock,PZERO|PCATCH'));
assert(ssp.includes('if(done!=gnss_pending || idle_us!=4000)return idle_us'));
assert(ssp.includes('!ssp.enabled && !gnss_readers && !gnss_req_busy && !gnss_req_pending'));
assert(gnss.includes('if(e!=ENOTTY && e!=ENOTSUP)'));
assert(gnss.includes('p126_retry_at=t+0.25'));
const bt=get('drivers/bluetooth/bcm4361/hakios.bcm4361_bt.c'),abox=get('drivers/audio/abox/hakios.exynos_abox.c');
assert(bt.includes('sleep_ms(1)'));
assert(abox.includes('delay_for_interval(POLL_MS, NSEC_PER_MSEC)'));
assert(abox.includes('hakios_p126_audio_get()==1 && sink_head==sink_tail'));
const hal=get('userland/src/audio_plugin.c'),audio=get('drivers/audio/hakios.audio.c');
for(const s of [hal,audio])assert(s.includes("P126_AUDIO_ACTIVITY _IOW('r',3,uint32_t)"));
assert(hal.includes('if(d->object==OBJECT_SPEAKER)p126_audio_activity(2)'));
assert(hal.includes('if(d->object==OBJECT_SPEAKER)p126_audio_activity(1)'));
// Model lease mask/expiry and read readiness deadline bounds.
for(let mask=1;mask<=63;mask++){
 const end=120000n&~63n,word=end|BigInt(mask);
 assert.equal(word&63n,BigInt(mask));
 assert.equal(word&~63n,end);
 assert(119000n<end && !(120000n<end));
}
for(const remain of [.001,.002,.05,.1,2]){
 const ms=remain>=.1?100:Math.floor(remain*1000);
 assert(ms>=1&&ms<=100&&ms/1000<=remain);
}
console.log('P12.6 source contracts and bounded lease/deadline model PASS (not native compile)');
