/* SPDX-License-Identifier: MIT
 * P4 Idle Compositor & CPU Residency Lab.
 * Included by hakios.exynos_cpufreq.c AFTER SYSCTL_DECL(_kern_hakios).
 * Diagnostic only: NO CPU suspend, CPD, SICD, WFI, hotplug, or clock writes.
 * This measures XNU scheduler-accounted idle (ml_cpu_idle_time). It is
 * NOT an architectural CPU power-state or rail-energy measurement.
 */
#include "hakios.p4_idle_math.h"
static uint32_t p4_mode;          /* 0 off; 1 read-only; 2 shadow classification */
static uint32_t p4_phase;         /* log marker; never changes hardware */
static uint32_t p4_threads;       /* poster_cost: 0 off, 1 backboard, 2 plus SpringBoard/PosterBoard */
static uint32_t p4_log_ms = 5000; /* 1..30s; runs in EXISTING governor thread */
static uint64_t p4_last_at, p4_previous_ticks[S9_P4_CORE_COUNT];
static uint32_t p4_core_pct[S9_P4_CORE_COUNT];
static uint32_t p4_idle_valid_mask, p4_idle_off_mask, p4_little_pct, p4_big_pct;
static uint32_t p4_little_online, p4_big_online, p4_last_interval_ms, p4_sample_count;
static uint32_t p4_samples_skipped, p4_last_frames, p4_last_touches;
static uint32_t p4_little_mhz, p4_big_mhz;
static uint64_t p4_frame_count, p4_touch_count, p4_last_frame;
static uint64_t p4_frame_prev, p4_frame_gap_bins[4], p4_gap17, p4_gap50, p4_gap120;
static uint64_t p4_last_gap_bins[4];
static uint64_t p4_last_idle_up_ms;

static void
p4_cpu_reset_baseline(void)
{
    /* sysctl handlers can run off the governor thread; p4_last_at=0
     * makes the next governor pass establish a new baseline rather than
     * subtracting timestamps from two different test phases. */
    __atomic_store_n(&p4_last_at, 0, __ATOMIC_RELEASE);
    p4_sample_count=0;
    p4_idle_valid_mask=0;
    __atomic_store_n(&p4_frame_prev, 0, __ATOMIC_RELEASE);
    __atomic_exchange_n(&p4_frame_count, 0, __ATOMIC_RELAXED);
    __atomic_exchange_n(&p4_touch_count, 0, __ATOMIC_RELAXED);
    for (unsigned i=0; i<4; i++)
        __atomic_exchange_n(&p4_frame_gap_bins[i], 0, __ATOMIC_RELAXED);
}

/* Mode is intentionally NOT a cpu-policy switch. Mode 2 merely adds
 * userland shadow classifications; the kernel's idle sampling is identical. */
static int
p4_mode_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p4_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr == USER_ADDR_NULL) return err;
    if(value<0 || value>2) return EINVAL;
    if(!__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE)) return ENODEV;
    nanoseconds_to_absolutetime(17ull*NSEC_PER_MSEC,&p4_gap17);
    nanoseconds_to_absolutetime(50ull*NSEC_PER_MSEC,&p4_gap50);
    nanoseconds_to_absolutetime(120ull*NSEC_PER_MSEC,&p4_gap120);
    __atomic_store_n(&p4_mode,(uint32_t)value,__ATOMIC_RELEASE);
    p4_cpu_reset_baseline();
    say("p4: mode=%d (0=off 1=read-only 2=shadow; NO frame skip, no CPU power writes)\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p4_mode,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL,0,
    p4_mode_sysctl,"I","P4 read-only diagnostics 0 off, 1 passive, 2 shadow; no frame/power modifications");

static int
p4_phase_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p4_phase,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    uint64_t ns;
    if(err || req->newptr == USER_ADDR_NULL) return err;
    if(value<0 || value>99) return EINVAL;
    __atomic_store_n(&p4_phase,(uint32_t)value,__ATOMIC_RELEASE);
    p4_cpu_reset_baseline();
    absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    say("p4: PHASE=%d up=%llums (marker only; next CPU window rebased)\n",
        value,ns/NSEC_PER_MSEC);
    return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p4_phase,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,NULL,0,
    p4_phase_sysctl,"I","P4 test phase 0-99 marker only, no GPU/CPU writes");

static int
p4_threads_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p4_threads,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr == USER_ADDR_NULL) return err;
    if(value<0 || value>2) return EINVAL;
    __atomic_store_n(&p4_threads,(uint32_t)value,__ATOMIC_RELEASE);
    say("p4: userland per-thread sampling=%d (0 none 1 backboardd 2 plus SpringBoard/PosterBoard)\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p4_threads,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,NULL,0,
    p4_threads_sysctl,"I","P4 thread sampling in already-running poster_cost, mode0 disables");

static int
p4_interval_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p4_log_ms,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr == USER_ADDR_NULL) return err;
    if(value<1000 || value>30000) return EINVAL;
    __atomic_store_n(&p4_log_ms,(uint32_t)value,__ATOMIC_RELEASE);
    p4_cpu_reset_baseline();
    say("p4: CPU idle sampling interval=%dms, in existing cpufreq governor worker\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p4_log_ms,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,NULL,0,
    p4_interval_sysctl,"I","P4 scheduler idle sample window 1000..30000ms, default5000");

static int
p4_stats_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[360];
    snprintf(line,sizeof(line),
        "phase=%u mode=%u threads=%u interval_ms=%u valid=%u samples=%u cpu_idle_lit_pct=%u cpu_idle_big_pct=%u lit_online=%u big_online=%u exit_mask=%#x cores_valid=%#x frames_last=%u touches_last=%u cpu_freq_lit_mhz=%u cpu_freq_big_mhz=%u deeper_state=unmeasured",
        __atomic_load_n(&p4_phase,__ATOMIC_RELAXED),
        __atomic_load_n(&p4_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p4_threads,__ATOMIC_RELAXED),
        __atomic_load_n(&p4_log_ms,__ATOMIC_RELAXED),
        p4_sample_count ? 1u : 0u,p4_sample_count,
        p4_little_pct,p4_big_pct,p4_little_online,p4_big_online,
        hakios_cpu_exit_mask,p4_idle_valid_mask,p4_last_frames,p4_last_touches,
        p4_little_mhz,p4_big_mhz);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, p4_stats,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,NULL,0,
    p4_stats_sysctl,"A","Read-only per-cluster scheduler-idle occupancy and frame/touch counts");

static void
p4_cpu_probe(uint64_t now)
{
    uint64_t span,ns;
    const ml_topology_info_t *topology;
    uint32_t valid=0, off=0;
    uint32_t lsum=0,bsum=0,lcount=0,bcount=0;
    uint32_t values[S9_P4_CORE_COUNT];
    uint32_t frames,touches;
    uint64_t gaps[4];
    uint32_t mode=__atomic_load_n(&p4_mode,__ATOMIC_ACQUIRE);
    uint32_t interval=__atomic_load_n(&p4_log_ms,__ATOMIC_RELAXED);
    uint64_t last=__atomic_load_n(&p4_last_at,__ATOMIC_ACQUIRE);
    if(!mode || !__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE))
        return;
    if(last) {
        uint64_t elapsed;
        if(now<last) return;
        absolutetime_to_nanoseconds(now-last,&elapsed);
        if(elapsed<(uint64_t)interval*NSEC_PER_MSEC)
            return;
    }
    topology=ml_get_topology_info();
    span=last && now>=last?now-last:0;
    for(unsigned i=0;i<S9_P4_CORE_COUNT;i++){
        uint64_t idle=0;
        bool readok=i<topology->num_cpus && ml_cpu_idle_time((int)i,&idle);
        bool online=(i>=32 || (hakios_cpu_exit_mask&(1u<<i))==0);
        unsigned pct;
        if(!online) off|=(1u<<i);
        pct=s9_p4_idle_percent(p4_previous_ticks[i],idle,span,
            last && readok && idle != 0 && p4_previous_ticks[i] != 0,online);
        if(readok) p4_previous_ticks[i]=idle;
        values[i]=pct;
        p4_core_pct[i]=pct;
        if(pct==S9_P4_NO_SAMPLE)continue;
        valid|=(1u<<i);
        uint32_t aff=(topology->cpus[i].phys_id>>8)&0xff;
        if(aff==clusters[1].affinity){bsum+=pct;bcount++;}
        else if(aff==clusters[0].affinity){lsum+=pct;lcount++;}
    }
    __atomic_store_n(&p4_last_at,now,__ATOMIC_RELEASE);
    if(!last){return;} /* first probe supplies baseline only */
    p4_idle_valid_mask=valid;
    p4_idle_off_mask=off;
    p4_little_online=lcount;p4_big_online=bcount;
    p4_little_pct=lcount?lsum/lcount:S9_P4_NO_SAMPLE;
    p4_big_pct=bcount?bsum/bcount:S9_P4_NO_SAMPLE;
    p4_little_mhz=clusters[0].cur/1000;
    p4_big_mhz=clusters[1].cur/1000;
    frames=(uint32_t)__atomic_exchange_n(&p4_frame_count,0,__ATOMIC_RELAXED);
    touches=(uint32_t)__atomic_exchange_n(&p4_touch_count,0,__ATOMIC_RELAXED);
    for(unsigned i=0;i<4;i++)
        gaps[i]=__atomic_exchange_n(&p4_frame_gap_bins[i],0,__ATOMIC_RELAXED);
    absolutetime_to_nanoseconds(span,&ns);
    p4_last_interval_ms=(uint32_t)(ns/NSEC_PER_MSEC);
    p4_last_frames=frames;p4_last_touches=touches;
    p4_sample_count++;
    uint64_t up_ns;
    absolutetime_to_nanoseconds(now,&up_ns);
    p4_last_idle_up_ms=up_ns/NSEC_PER_MSEC;
    say("p4-cpu: up=%llums phase=%u mode=%u span_ms=%u idle_lit=%u%% idle_big=%u%% lit_online=%u big_online=%u exit=%#x valid=%#x mhz=%u/%u frames=%u touches=%u gap_lt17=%llu gap_17_50=%llu gap_50_120=%llu gap_ge120=%llu deep=unmeasured\n",
        p4_last_idle_up_ms,__atomic_load_n(&p4_phase,__ATOMIC_RELAXED),mode,
        p4_last_interval_ms,p4_little_pct,p4_big_pct,lcount,bcount,off,valid,
        p4_little_mhz,p4_big_mhz,frames,touches,gaps[0],gaps[1],gaps[2],gaps[3]);
    say("p4-core: up=%llums phase=%u idle_pct=%u,%u,%u,%u,%u,%u,%u,%u (255=unavailable; scheduler idle ONLY, not WFI or CPD)\n",
        p4_last_idle_up_ms,__atomic_load_n(&p4_phase,__ATOMIC_RELAXED),
        values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7]);
}
