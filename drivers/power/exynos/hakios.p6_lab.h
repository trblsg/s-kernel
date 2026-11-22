/* SPDX-License-Identifier: MIT
 * S9 P6 unified diagnostics. No CPU/GPU/voltage/clock/hotplug writes.
 * Included by Exynos9810 governor (the existing timer/worker): no new thread.
 * P6.1 is EVENT/IDLE PROXY only, NOT global GIC IRQ, WFI or CPD residency.
 * P6.2 and P6.5 run in existing poster_cost / EAGL userland, via sysctl.
 * P6.4 uses existing separate G3D Lab with fail-closed reset.
 */
#ifndef HAKIOS_S9_P6_LAB_H
#define HAKIOS_S9_P6_LAB_H
#include "hakios.p6_policy.h"
#define P6_F_WAKE  1u
#define P6_F_PROC  2u
#define P6_F_SHADOW 4u
#define P6_F_G3D 8u
#define P6_F_CA 16u
static uint32_t p6_mode, p6_phase, p6_flags=31u, p6_log_ms=5000u;
/* P6.6/.7/.9 independent opt-in read-only instrumentation toggles.
 * P6.8 resides in Mali/G3D because it owns the PMU and work timestamps. */
static uint32_t p66_mode, p67_mode, p69_mode;
/* P6.10-12 read-only kernel toggles; userland reads P6.11/12 via sysctl. */
static uint32_t p610_mode,p611_mode,p612_mode;
SYSCTL_UINT(_kern_hakios, OID_AUTO, p66_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p66_mode, 0, "P6.6 target/damage/frame-candidate read-only 60-frame diagnostics");
SYSCTL_UINT(_kern_hakios, OID_AUTO, p67_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p67_mode, 0, "P6.7 EAGL per-slot CPU wall timing 60-frame diagnostics");
SYSCTL_UINT(_kern_hakios, OID_AUTO, p69_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p69_mode, 0, "P6.9 USB/touch/key periodic source attribution; no poll policy changes");
SYSCTL_UINT(_kern_hakios, OID_AUTO, p610_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p610_mode, 0, "P6.10 Scion state/cadence shadow only; never change governor");
SYSCTL_UINT(_kern_hakios, OID_AUTO, p611_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p611_mode, 0, "P6.11 sampled IOSurface checksum-path attribution, no extra hashes");
SYSCTL_UINT(_kern_hakios, OID_AUTO, p612_mode, CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p612_mode, 0, "P6.12 EAGL vs display-fps presentation diagnostics, no frame writes");
static uint64_t p6_last_at, p6_last_idle[8];
static uint32_t p6_windows, p6_touch_events, p6_frame_events, p6_boost_requests;
static uint32_t p6_last_lit=255u, p6_last_big=255u, p6_last_windows;
static uint32_t p6_last_boost, p6_last_frames, p6_last_touches, p6_samples;
static struct s9_p6_shadow p6_shadow;
static int32_t p6_last_big_mc=-273000, p6_last_gpu_mc=-273000;

static void
p6_rebase(void)
{
    /* Called on serial phase/config change. No implicit P4/P5 reset. */
    __atomic_store_n(&p6_last_at,0,__ATOMIC_RELEASE);
    __atomic_exchange_n(&p6_windows,0,__ATOMIC_RELAXED);
    __atomic_exchange_n(&p6_touch_events,0,__ATOMIC_RELAXED);
    __atomic_exchange_n(&p6_frame_events,0,__ATOMIC_RELAXED);
    __atomic_exchange_n(&p6_boost_requests,0,__ATOMIC_RELAXED);
}

static int
p6_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p6_mode,__ATOMIC_RELAXED);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>2)return EINVAL;
    if(v && !__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE))return ENODEV;
    __atomic_store_n(&p6_mode,(uint32_t)v,__ATOMIC_RELEASE);
    p6_rebase();
    say("p6: mode=%d 0=off 1=passive 2=detailed (no CPU power, clock, GPU or frame writes)\n",v);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p6_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p6_mode_sysctl,"I","P6 unified passive telemetry: 0 off, 1 summary, 2 detailed");

static int
p6_phase_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p6_phase,__ATOMIC_RELAXED);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    uint64_t ns;
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>99)return EINVAL;
    __atomic_store_n(&p6_phase,(uint32_t)v,__ATOMIC_RELEASE);
    p6_rebase();
    absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    say("p6: PHASE=%d up=%llums flags=%u (marker only; rebase next sample)\n",
        v,ns/NSEC_PER_MSEC,__atomic_load_n(&p6_flags,__ATOMIC_RELAXED));
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p6_phase,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p6_phase_sysctl,"I","P6 phase 0..99; resets observation window only");

static int
p6_flags_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p6_flags,__ATOMIC_RELAXED);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>31)return EINVAL;
    __atomic_store_n(&p6_flags,(uint32_t)v,__ATOMIC_RELEASE);
    p6_rebase();
    say("p6: flags=%d wake1 proc2 shadow4 g3d8 CA16 (read-only; G3D gate separately controlled)\n",v);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p6_flags,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p6_flags_sysctl,"I","P6 feature mask 1 wake proxy,2 process,4 shadow,8 G3D,16 CA");

static int
p6_interval_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p6_log_ms,__ATOMIC_RELAXED);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<1000 || v>30000)return EINVAL;
    __atomic_store_n(&p6_log_ms,(uint32_t)v,__ATOMIC_RELEASE);
    p6_rebase();
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p6_log_ms,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p6_interval_sysctl,"I","P6 existing-governor sampling interval 1000..30000ms");

static int
p6_stats_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char out[360];
    snprintf(out,sizeof(out),
        "phase=%u mode=%u flags=%u interval_ms=%u samples=%u idle_lit=%u idle_big=%u gov_windows=%u boosts=%u touches=%u frames=%u big_mc=%d gpu_mc=%d big_shadow=%u gpu_shadow=%u park_candidate=%u irq=unmeasured deep=unmeasured gate=separate_g3d_lab",
        __atomic_load_n(&p6_phase,__ATOMIC_RELAXED),
        __atomic_load_n(&p6_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p6_flags,__ATOMIC_RELAXED),
        __atomic_load_n(&p6_log_ms,__ATOMIC_RELAXED),
        p6_samples,p6_last_lit,p6_last_big,p6_last_windows,
        p6_last_boost,p6_last_touches,p6_last_frames,
        p6_last_big_mc,p6_last_gpu_mc,p6_shadow.big_hot,
        p6_shadow.gpu_hot,p6_shadow.park_candidate);
    return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p6_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p6_stats_sysctl,"A","P6 last wake-proxy and thermal policy shadow sample");

static void
p6_probe(uint64_t now)
{
    uint32_t mode=__atomic_load_n(&p6_mode,__ATOMIC_ACQUIRE);
    uint32_t flags=__atomic_load_n(&p6_flags,__ATOMIC_RELAXED);
    uint64_t last=__atomic_load_n(&p6_last_at,__ATOMIC_ACQUIRE), ns=0;
    uint32_t lit_sum=0,big_sum=0,lit_n=0,big_n=0;
    uint32_t no_sample=0, touches,frames,boosts,windows,gap_ms=0;
    const ml_topology_info_t *topo;
    if(!mode || !flags)return;
    if(last && now>=last) {
        absolutetime_to_nanoseconds(now-last,&ns);
        if(ns<(uint64_t)__atomic_load_n(&p6_log_ms,__ATOMIC_RELAXED)*NSEC_PER_MSEC)
            return;
    }
    topo=ml_get_topology_info();
    for(unsigned i=0;i<8;i++){
        uint64_t idle=0;
        bool ok=i<topo->num_cpus && !(hakios_cpu_exit_mask&(1u<<i)) &&
            ml_cpu_idle_time((int)i,&idle);
        if(ok && last && now>last && idle>=p6_last_idle[i] && p6_last_idle[i]){
            uint64_t elapsed=now-last, change=idle-p6_last_idle[i];
            uint32_t pct=(uint32_t)(change>=elapsed?100:(change*100)/elapsed);
            unsigned aff=(topo->cpus[i].phys_id>>8)&0xff;
            if(aff==clusters[0].affinity){lit_sum+=pct;lit_n++;}
            else if(aff==clusters[1].affinity){big_sum+=pct;big_n++;}
        }else no_sample++;
        p6_last_idle[i]=ok?idle:0;
    }
    __atomic_store_n(&p6_last_at,now,__ATOMIC_RELEASE);
    touches=__atomic_exchange_n(&p6_touch_events,0,__ATOMIC_RELAXED);
    frames=__atomic_exchange_n(&p6_frame_events,0,__ATOMIC_RELAXED);
    boosts=__atomic_exchange_n(&p6_boost_requests,0,__ATOMIC_RELAXED);
    windows=__atomic_exchange_n(&p6_windows,0,__ATOMIC_RELAXED);
    if(!last)return; /* first pass only establishes a timebase */
    p6_last_lit=lit_n?lit_sum/lit_n:255;
    p6_last_big=big_n?big_sum/big_n:255;
    p6_last_windows=windows;p6_last_boost=boosts;
    p6_last_touches=touches;p6_last_frames=frames;
    p6_samples++;
    uint64_t up_ns=0;
    absolutetime_to_nanoseconds(now,&up_ns);
    if(flags&P6_F_WAKE){
        say("p6-wake: up=%llums phase=%u span_ms=%llu gov_loops=%u boost_requests=%u touches=%u presents=%u idle_lit=%u idle_big=%u missing=%u source=governor_and_scheduler_proxy_not_IRQ_or_wakeup_counts\n",
            up_ns/NSEC_PER_MSEC,__atomic_load_n(&p6_phase,__ATOMIC_RELAXED),
            ns/NSEC_PER_MSEC,windows,boosts,touches,frames,
            p6_last_lit,p6_last_big,no_sample);
        if(mode>=2)
            say("p6-wake-detail: up=%llums phase=%u cpu_mask=%#x lit_mhz=%u big_mhz=%u lit_busy=%u big_busy=%u irq=unmeasured timer_callout=unmeasured cpdsicd=unmeasured\n",
                up_ns/NSEC_PER_MSEC,__atomic_load_n(&p6_phase,__ATOMIC_RELAXED),
                hakios_cpu_exit_mask,clusters[0].cur/1000,clusters[1].cur/1000,
                clusters[0].load,clusters[1].load);
    }
    if(flags&P6_F_SHADOW){
        uint64_t fg=__atomic_load_n(&frame_gap,__ATOMIC_RELAXED);
        if(fg){uint64_t g=0;absolutetime_to_nanoseconds(fg,&g);gap_ms=(uint32_t)(g/NSEC_PER_MSEC);}
        p6_last_big_mc=hakios_exynos_tmu_zone_mc(0);
        p6_last_gpu_mc=hakios_exynos_tmu_zone_mc(2);
        p6_shadow=s9_p6_shadow_eval(p6_shadow,p6_last_big_mc,p6_last_gpu_mc,
            clusters[1].load,clusters[0].load,touches,frames,gap_ms);
        say("p6-shadow: up=%llums phase=%u big_mc=%d gpu_mc=%d m3_load=%u lit_load=%u mhz=%u/%u frames=%u gap_ms=%u park_candidate=%u big_step=%u gpu_step=%u changed=%u writes=0\n",
            up_ns/NSEC_PER_MSEC,__atomic_load_n(&p6_phase,__ATOMIC_RELAXED),
            p6_last_big_mc,p6_last_gpu_mc,clusters[1].load,
            clusters[0].load,clusters[0].cur/1000,clusters[1].cur/1000,
            frames,gap_ms,p6_shadow.park_candidate,p6_shadow.big_hot,
            p6_shadow.gpu_hot,p6_shadow.changed);
    }
}
#endif
