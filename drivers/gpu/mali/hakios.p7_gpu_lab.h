/* P7 G3D / SoC idle ring: no NEW worker, use existing G3D 5s monitor.
 * Only read PMU/CMU outside the G3D domain, previous job/frame counters,
 * BUCK6's last separately sampled value; never MMIO into powered-off GPU.
 * This data survives unplugging serial until ring wraps (kernel RAM only).
 */
#ifndef HAKIOS_P7_GPU_LAB_H
#define HAKIOS_P7_GPU_LAB_H
/* P8 one-boot offline correlation: 512 x 5s ~= 42.7min of GPU PMU/PLL. */
#define P7_GPU_RING 512u
struct p7_gpu_row {
    volatile uint64_t seq;
    uint64_t uptime_ms, frame_age_ms, job_age_ms, off_total_ms;
    uint32_t phase, domain, pll, mux, awake, clock_mhz, rail_valid, rail_mv,
             gate, fault, off_count, wake_count, wake_fail, gpu_safe, tz_ok;
    int32_t g3d_temp_mc;
};
static struct p7_gpu_row p7_gpu_rows[P7_GPU_RING];
static uint32_t p7_gpu_mode, p7_gpu_phase, p7_gpu_slot, p7_gpu_count;
static uint64_t p7_gpu_head;
/* Host-disconnect protection for the NEW P7 experimental mode only.
 * Existing non-P7 G3D Lab semantics are untouched. The same sleeping
 * 5-second G3D monitor owns this 120s watchdog, no new timer/thread. */
static uint64_t p7_gpu_gate_until;
static void
p7_gpu_gate_watchdog(uint64_t now)
{
    /* Called WITH mali_job_lock, before reading the PMU snapshot. */
    if(g3d_lab_gate!=3){
        if(p7_gpu_gate_until){
            p7_gpu_gate_until=0;
            say("p7-gpu: gate_watchdog=disarmed real_gate_disabled=1\n");
        }
        return;
    }
    if(!p7_gpu_gate_until){
        if(!__atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED))return;
        clock_interval_to_deadline(120000,NSEC_PER_MSEC,&p7_gpu_gate_until);
        say("p7-gpu: gate_watchdog=ARMED max_seconds=120 "
            "automatic_restore_even_without_COM=1\n");
        return;
    }
    if(now<p7_gpu_gate_until)return;
    if(!g3d_awake && !g3d_power_on()){
        g3d_lab_trip(7);
        p7_gpu_gate_until=UINT64_MAX;
        say("p7-gpu: gate_watchdog=RESTORE_FAILED gpu_safe=0 "
            "reboot_rollback_needed=1\n");
        return;
    }
    if(!g3d_lab_gpu_safe || !g3d_lab_protection_ok){
        g3d_lab_trip(7);
        p7_gpu_gate_until=UINT64_MAX;
        say("p7-gpu: gate_watchdog=RESTORE_FAILED invalid_gpu_or_TZ "
            "reboot_rollback_needed=1\n");
        return;
    }
    g3d_sleep=0;
    g3d_lab_gate=0;
    p7_gpu_gate_until=0;
    say("p7-gpu: gate_watchdog=RESTORED gate=0 domain=ON "
        "gpu_safe=1 tz_ok=1 voltage_policy_unchanged=1\n");
}
static int
p7_gpu_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>1)return EINVAL;
    if(value && (pmu==0 || cmu_g3d==0))return ENODEV;
    __atomic_store_n(&p7_gpu_mode,(uint32_t)value,__ATOMIC_RELEASE);
    if(value)g3d_lab_monitor_start();
    say("p7-gpu: mode=%d ring=%u sampled_by=existing_G3D_5s_worker "
        "power_changes=0 GPU_MMIO=0\n",value,P7_GPU_RING);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_gpu_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_gpu_mode_sysctl,"I","P7 PMU/PLL/clock and live off-residency ring");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p7_gpu_phase,
    CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,&p7_gpu_phase,0,
    "P7 GPU marker (0-99) does NOT switch the G3D domain");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p7_gpu_slot,
    CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,&p7_gpu_slot,0,
    "P7 GPU history offset: 0 latest, 1 previous, etc");
static int
p7_gpu_stats_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[158];
    snprintf(line,sizeof(line),"mode=%u phase=%u ring=%u samples=%u "
        "head_seq=%llu slot=%u gpu_mmio=never sample_worker=existing",
        __atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p7_gpu_phase,__ATOMIC_RELAXED),
        P7_GPU_RING,p7_gpu_count,(unsigned long long)p7_gpu_head,p7_gpu_slot);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_gpu_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_gpu_stats_sysctl,"A","P7 GPU history status (safe when PMU domain off)");
static int
p7_gpu_record_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[248];
    const uint64_t head=__atomic_load_n(&p7_gpu_head,__ATOMIC_ACQUIRE);
    const uint32_t slot=__atomic_load_n(&p7_gpu_slot,__ATOMIC_RELAXED);
    if(!head || slot>=P7_GPU_RING || slot>=p7_gpu_count){
        snprintf(line,sizeof(line),"unavailable=1 head=%llu slot=%u samples=%u",
                 (unsigned long long)head,slot,p7_gpu_count);
    }else{
        struct p7_gpu_row *r=&p7_gpu_rows[(head-1-slot)%P7_GPU_RING];
        const uint64_t seq=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        const struct p7_gpu_row copy=*r;
        const uint64_t seq2=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        if(!seq || seq!=seq2)
            snprintf(line,sizeof(line),"busy=1 head=%llu slot=%u",
                     (unsigned long long)head,slot);
        else
            snprintf(line,sizeof(line),"seq=%llu up_ms=%llu phase=%u domain=%u pll=%u "
                "awake=%u clock=%u rail_valid=%u rail_mv=%u "
                "frame_age=%llu job_age=%llu off_ms=%llu off=%u wake=%u fail=%u "
                "gate=%u fault=%u gpu_safe=%u tz_ok=%u g3d_mc=%d",
                (unsigned long long)seq,(unsigned long long)copy.uptime_ms,
                copy.phase,copy.domain,copy.pll,copy.awake,copy.clock_mhz,
                copy.rail_valid,copy.rail_mv,
                (unsigned long long)copy.frame_age_ms,
                (unsigned long long)copy.job_age_ms,
                (unsigned long long)copy.off_total_ms,copy.off_count,
                copy.wake_count,copy.wake_fail,copy.gate,copy.fault,
                copy.gpu_safe,copy.tz_ok,copy.g3d_temp_mc);
    }
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_gpu_record,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_gpu_record_sysctl,"A","P7 selected G3D PMU historical snapshot");

static void
p7_gpu_store(uint64_t now,uint64_t up_ns,uint32_t status,uint32_t pll,
    uint32_t mux,bool awake)
{
    if(__atomic_load_n(&p7_gpu_mode,__ATOMIC_RELAXED)!=1)return;
    const uint64_t last_frame=__atomic_load_n(&gov_frame_last,__ATOMIC_RELAXED);
    const uint64_t last_job=__atomic_load_n(&g3d_last_busy,__ATOMIC_RELAXED);
    uint64_t off_ns=0;
    if(!awake && g3d_off_at && now>=g3d_off_at)
        absolutetime_to_nanoseconds(now-g3d_off_at,&off_ns);
    const uint64_t head=__atomic_load_n(&p7_gpu_head,__ATOMIC_RELAXED);
    struct p7_gpu_row *r=&p7_gpu_rows[head%P7_GPU_RING];
    __atomic_store_n(&r->seq,0,__ATOMIC_RELEASE);
    r->uptime_ms=up_ns/NSEC_PER_MSEC;
    r->phase=__atomic_load_n(&p7_gpu_phase,__ATOMIC_RELAXED);
    r->domain=(status&G3D_ON)==G3D_ON?1u:0u;
    r->pll=(pll&(PLL_ENABLE|PLL_STABLE))==(PLL_ENABLE|PLL_STABLE)?1u:0u;
    r->mux=(mux&MUX_SEL)?1u:0u;r->awake=awake?1u:0u;
    r->clock_mhz=r->domain&&gov_level<GOV_LEVELS?g3d_rates[gov_level].mhz:0;
    r->rail_valid=g3d_lab_rail_valid?1u:0u;
    r->rail_mv=r->rail_valid?s9_g3d_lab_mv_from_vsel(g3d_lab_rail_vsel):0;
    r->frame_age_ms=last_frame?g3d_lab_age_ms(now,last_frame):0;
    r->job_age_ms=last_job?g3d_lab_age_ms(now,last_job):0;
    r->off_total_ms=(g3d_lab_off_residency_ns+off_ns)/NSEC_PER_MSEC;
    r->off_count=g3d_lab_off_count;r->wake_count=g3d_lab_wake_count;
    r->wake_fail=g3d_lab_wake_failed;r->gate=g3d_lab_gate;
    r->fault=g3d_lab_fault;r->gpu_safe=g3d_lab_gpu_safe?1u:0u;
    r->tz_ok=g3d_lab_protection_ok?1u:0u;
    r->g3d_temp_mc=hakios_exynos_tmu_zone_mc(2);
    __atomic_store_n(&r->seq,head+1,__ATOMIC_RELEASE);
    __atomic_store_n(&p7_gpu_head,head+1,__ATOMIC_RELEASE);
    if(p7_gpu_count<P7_GPU_RING)p7_gpu_count++;
    say("p7-gpu: up=%llums phase=%u domain=%u pll=%u awake=%u "
        "clock=%uMHz frame_age_ms=%llu job_age_ms=%llu "
        "off_total_ms=%llu off=%u wake=%u wake_fail=%u "
        "gate=%u fault=%u gpu_safe=%u tz_ok=%u "
        "rail_valid=%u rail_mv=%u g3d_mc=%d source=PMU_CMU_5s_not_watts\n",
        (unsigned long long)r->uptime_ms,r->phase,r->domain,r->pll,
        r->awake,r->clock_mhz,(unsigned long long)r->frame_age_ms,
        (unsigned long long)r->job_age_ms,
        (unsigned long long)r->off_total_ms,r->off_count,r->wake_count,
        r->wake_fail,r->gate,r->fault,r->gpu_safe,r->tz_ok,
        r->rail_valid,r->rail_mv,r->g3d_temp_mc);
}
#endif
