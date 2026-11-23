/* S9 P8: one-build read-only SoC timeline.
 * Included by the existing cpufreq governor thread (no new timer/thread).
 * All 10 streams are enabled by a Windows host orchestrator using existing
 * P4/P6/P7 observers as well as this consolidated ring.
 *
 * Proven unsafe P7 BIG CPU_OFF remains forbidden at BOTH sysctl and
 * GenericARMPE entry. P8 never calls processor_exit, CPU_OFF, CPU_ON,
 * CPU_SUSPEND, any PMU write, power rail write, or a G3D MMIO read.
 *
 * 255% idle, unavailable PMU, unobserved PSCI and missing battery data
 * are explicit UNKNOWNs, not zeros or physical idle evidence.
 */
#ifndef HAKIOS_P8_UNIFIED_LAB_H
#define HAKIOS_P8_UNIFIED_LAB_H
#include <sys/hakios.p8_soc_snapshot.h>
#define P8_RING 512u
#define P8_ALL_FLAGS 1023u
#define P8_UNAVAILABLE 255u
/* Bit 0 CPU, 1 PSCI, 2 PMU, 3 IRQ/USB/touch, 4 thermal/CPU DVFS,
 * 5 G3D, 6 display, 7 compositor/HI2, 8 charge/PMIC, 9 context.
 * PSCI read-only SMC occurs only via explicit p8_psci_stats sysctl.
 * G3D, display, compositor and battery use EXISTING driver observers;
 * their independent data must be phase-correlated by the host. */
static uint32_t p8_mode,p8_flags,p8_phase,p8_slot,p8_count,p8_interval_ms=5000;
static uint32_t p8_big_cap_khz; /* 0=stock; opt-in lower-only for thermal A/B/A */
static uint64_t p8_head,p8_last_at,p8_idle_prev[8],p8_usb_prev,p8_touch_prev;
static bool p8_usb_has_prev,p8_touch_has_prev;
static uint32_t p8_psci_cpu=3;
struct p8_row {
    volatile uint64_t seq;
    uint64_t up_ms, usb_delta, touch_delta;
    uint32_t phase, flags, disp, lit_idle, big_idle, lit_on, big_on, exit_mask;
    uint32_t lit_mhz,big_mhz,lit_cap_mhz,big_cap_mhz;
    int32_t big_mc,lit_mc,g3d_mc,battery_c;
    uint32_t pmu_valid,pmu_on_mask,pmu_cfg_mask,noncpu_status,l2_status;
    uint32_t usb_valid,touch_valid;
};
static struct p8_row p8_rows[P8_RING];
static void p8_reset_delta(void)
{
    __atomic_store_n(&p8_last_at,0,__ATOMIC_RELEASE);
    p8_usb_has_prev=false;
    p8_touch_has_prev=false;
    for(unsigned i=0;i<8;i++)p8_idle_prev[i]=0;
}
static int p8_mode_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p8_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>2)return EINVAL;
    if(value && !__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE))return ENODEV;
    __atomic_store_n(&p8_mode,(uint32_t)value,__ATOMIC_RELEASE);
    p8_reset_delta();
    say("p8: mode=%d safe=read_only no_CPU_OFF=1 ring=%u reused_gov=1\n",value,P8_RING);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_mode_sysctl,"I","P8 unified 0 off,1 passive,2 detailed passive; no power writes");
static int p8_flags_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p8_flags,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>(int)P8_ALL_FLAGS)return EINVAL;
    __atomic_store_n(&p8_flags,(uint32_t)value,__ATOMIC_RELEASE);
    p8_reset_delta();
    say("p8: flags=%d groups=cpu1_psci2_pmu4_irq8_thermal16_gpu32_display64_compositor128_power256_context512 external_observers=separate\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_flags,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_flags_sysctl,"I","P8 source selection mask 0..1023, passive only");
static int p8_phase_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p8_phase,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>99)return EINVAL;
    __atomic_store_n(&p8_phase,(uint32_t)value,__ATOMIC_RELEASE);
    p8_reset_delta();
    say("p8: PHASE=%d reset_delta=1 power_change=0\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_phase,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_phase_sysctl,"I","P8 global phase 0..99; resets data deltas, never power");
static int p8_interval_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p8_interval_ms,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<5000 || value>30000)return EINVAL;
    __atomic_store_n(&p8_interval_ms,(uint32_t)value,__ATOMIC_RELEASE);
    p8_reset_delta();
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_interval_ms,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_interval_sysctl,"I","P8 existing-governor samples every 5..30 seconds");
/* Only a lower thermal-lab BIG cap, never a clock/voltage boost.
 * The existing governor's thermal and OCP ceilings remain additional
 * constraints. The host MUST restore 0 in cleanup. */
static int p8_big_cap_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p8_big_cap_khz,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value!=0 && value!=1469000 && value!=1586000 && value!=1690000)
        return EINVAL;
    if(value && !__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE))return ENODEV;
    __atomic_store_n(&p8_big_cap_khz,(uint32_t)value,__ATOMIC_RELEASE);
    say("p8: thermal_cap_big_khz=%d lower_only=1 voltage_unchanged=1\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_big_cap_khz,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_big_cap_sysctl,"I","P8 optional BIG thermal cap 0,1469000,1586000,1690000");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p8_slot,CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p8_slot,0,"P8 historical offset, 0 newest, max 511");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p8_psci_cpu,CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,
    &p8_psci_cpu,0,"P8 explicit read-only PSCI affinity query for CPU 3..6");
static int p8_stats_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char line[240];
    snprintf(line,sizeof(line),"mode=%u flags=%u phase=%u ring=%u samples=%u "
        "head_seq=%llu slot=%u interval_ms=%u legacy_cpu_off=blocked "
        "psci_affinity=explicit_only deep_C2_CPD_SICD=unmeasured",
        __atomic_load_n(&p8_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p8_flags,__ATOMIC_RELAXED),
        __atomic_load_n(&p8_phase,__ATOMIC_RELAXED),
        P8_RING,p8_count,(unsigned long long)p8_head,p8_slot,p8_interval_ms);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_stats_sysctl,"A","P8 unified status, no power control");
static int p8_record_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char line[400];
    const uint64_t head=__atomic_load_n(&p8_head,__ATOMIC_ACQUIRE);
    const uint32_t slot=__atomic_load_n(&p8_slot,__ATOMIC_RELAXED);
    if(!head || slot>=P8_RING || slot>=p8_count)
        snprintf(line,sizeof(line),"unavailable=1 head=%llu slot=%u count=%u",
            (unsigned long long)head,slot,p8_count);
    else {
        struct p8_row *r=&p8_rows[(head-1-slot)%P8_RING];
        const uint64_t before=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        struct p8_row copy=*r;
        const uint64_t after=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        if(!before || before!=after)
            snprintf(line,sizeof(line),"busy=1 head=%llu slot=%u",
                (unsigned long long)head,slot);
        else
            snprintf(line,sizeof(line),"seq=%llu up_ms=%llu ph=%u flags=%u disp=%u "
                "lit_on=%u big_on=%u idle=%u/%u freq=%u/%u cap=%u/%u "
                "temp_mc=%d/%d/%d batt_c=%d exit=%#x pmu_valid=%u "
                "pmu_on=%#x pmu_cfg=%#x noncpu=%#x l2=%#x "
                "usb_valid=%u usb=%llu touch_valid=%u touch=%llu "
                "psci=not_sampled_see_p8_psci_stats",
                (unsigned long long)copy.seq,(unsigned long long)copy.up_ms,
                copy.phase,copy.flags,copy.disp,copy.lit_on,copy.big_on,
                copy.lit_idle,copy.big_idle,copy.lit_mhz,copy.big_mhz,
                copy.lit_cap_mhz,copy.big_cap_mhz,
                copy.big_mc,copy.lit_mc,copy.g3d_mc,copy.battery_c,
                copy.exit_mask,copy.pmu_valid,copy.pmu_on_mask,copy.pmu_cfg_mask,
                copy.noncpu_status,copy.l2_status,copy.usb_valid,
                (unsigned long long)copy.usb_delta,copy.touch_valid,
                (unsigned long long)copy.touch_delta);
    }
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_record,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_record_sysctl,"A","P8 up to 512 retained rows, kernel RAM only");
static int p8_psci_stats_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char line[220];
    struct hakios_p8_psci_snapshot s={0};
    const uint32_t cpu=__atomic_load_n(&p8_psci_cpu,__ATOMIC_RELAXED);
    if(cpu<3 || cpu>6 || hakios_p8_psci_snapshot(cpu,&s)!=0 || !s.valid)
        snprintf(line,sizeof(line),"unavailable=1 cpu=%u valid_range=3..6",cpu);
    else
        snprintf(line,sizeof(line),"cpu=%u mpidr=%#x stage=%u on_attempts=%u "
            "off_attempts=%u off_returns=%u last_on=%d last_off=%d affinity=%lld "
            "affinity_off=1 caller_stage1_not_target_execution=1 read_only=1",
            s.logical_cpu,s.mpidr,s.stage,s.start_attempts,s.off_attempts,
            s.off_returns,s.last_cpu_on_status,s.last_cpu_off_status,(long long)s.affinity);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_psci_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_psci_stats_sysctl,"A","P8 explicit per-M3 read-only PSCI AFFINITY_INFO");
static int p8_pmu_stats_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char line[315];
    struct hakios_p8_m3_snapshot s={0};
    if(hakios_p8_m3_snapshot(&s)!=0 || !s.valid)
        snprintf(line,sizeof(line),"unavailable=1 m3_pmu_map=not_ready no_mmio=1");
    else
        snprintf(line,sizeof(line),"valid=1 config=%x,%x,%x,%x "
            "status=%x,%x,%x,%x noncpu=%x/%x l2=%x reset=%x "
            "reset_disable_soft=%x reset_disable_wdt=%x "
            "source=always_on_PMU no_power_writes=1",
            s.cpu_config[0],s.cpu_config[1],s.cpu_config[2],s.cpu_config[3],
            s.cpu_status[0],s.cpu_status[1],s.cpu_status[2],s.cpu_status[3],
            s.noncpu_config,s.noncpu_status,s.l2_status,s.reset_sequence,
            s.reset_disable_soft,s.reset_disable_wdt);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_pmu_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_pmu_stats_sysctl,"A","P8 detailed Cluster1 PMU snapshot; never powered-off-domain MMIO");

/* Explicitly requested rail observation. ACPM/PMIC values are register
 * readbacks, not a physical voltmeter; no polling on hot governor path. */
static int p8_rail_stats_sysctl(__unused struct sysctl_oid *oid,__unused void *a,
    __unused int b,struct sysctl_req *req)
{
    char line[280];
    struct hakios_p8_rails_snapshot s={0};
    int err=hakios_p8_rails_snapshot(&s);
    if(err || !s.valid){
        snprintf(line,sizeof(line),"unavailable=1 err=%d e=%d,%d,%d,%d,%d,%d source=S2MPS18_no_valid_voltmeter_data",
            err,s.err_lit,s.err_big,s.err_b2c,s.err_b2v,s.err_b3c,s.err_b3v);
        return SYSCTL_OUT(req,line,strlen(line)+1);
    }
    snprintf(line,sizeof(line),"valid=%u err=%d acpm_lit_khz=%u acpm_big_khz=%u "
        "b2ctrl=%#x b2vsel=%#x b2mv=%u b3ctrl=%#x b3vsel=%#x "
        "b3mv=%u e=%d,%d,%d,%d,%d,%d source=S2MPS18_VSEL_not_voltmeter",
        s.valid,err,s.lit_khz,s.big_khz,s.b2_ctrl,s.b2_vsel,
        300u+(uint32_t)s.b2_vsel*25u/4u,s.b3_ctrl,s.b3_vsel,
        300u+(uint32_t)s.b3_vsel*25u/4u,s.err_lit,s.err_big,s.err_b2c,
        s.err_b2v,s.err_b3c,s.err_b3v);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p8_rail_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p8_rail_stats_sysctl,"A","P8 demand-only BUCK2/BUCK3/ACPM readbacks");

static void p8_probe(uint64_t now)
{
    const uint32_t mode=__atomic_load_n(&p8_mode,__ATOMIC_ACQUIRE);
    const uint32_t flags=__atomic_load_n(&p8_flags,__ATOMIC_RELAXED);
    if(!mode || !flags)return;
    uint64_t old=__atomic_load_n(&p8_last_at,__ATOMIC_ACQUIRE),ns=0;
    if(old){
        if(now<=old)return;
        absolutetime_to_nanoseconds(now-old,&ns);
        if(ns<(uint64_t)p8_interval_ms*NSEC_PER_MSEC)return;
    }
    const ml_topology_info_t *topo=ml_get_topology_info();
    struct hakios_p8_m3_snapshot pmu={0};
    uint32_t lit_sum=0,big_sum=0,lit_n=0,big_n=0,lit_on=0,big_on=0;
    for(unsigned i=0;i<8;i++){
        uint64_t idle=0;
        const bool ok=i<topo->num_cpus && ml_cpu_idle_time((int)i,&idle);
        const bool online=i<topo->num_cpus &&
            !(hakios_cpu_exit_mask&(1u<<i));
        const bool big=i<topo->num_cpus &&
            ((topo->cpus[i].phys_id>>8)&0xff)==clusters[1].affinity;
        if(online){if(big)big_on++;else lit_on++;}
        const uint32_t pct=s9_p4_idle_percent(p8_idle_prev[i],idle,
            old?now-old:0,old && ok && idle && p8_idle_prev[i],online);
        if(ok)p8_idle_prev[i]=idle;
        if(pct!=S9_P4_NO_SAMPLE){
            if(big){big_sum+=pct;big_n++;}else{lit_sum+=pct;lit_n++;}
        }
    }
    __atomic_store_n(&p8_last_at,now,__ATOMIC_RELEASE);
    /* Never make a PSCI SMC from the high-frequency governor worker. */
    if(!old)return;
    if(flags & 4u)(void)hakios_p8_m3_snapshot(&pmu);
    uint64_t usb=0,touch=0;
    bool usb_ok=false,touch_ok=false;
    if(flags & 8u){
        usb_ok=p7_kernel_quad("kern.hakios.p6_usb_ticks",&usb);
        touch_ok=p7_kernel_quad("kern.hakios.p6_touch_polls",&touch);
    }
    uint64_t usb_delta=usb_ok&&p8_usb_has_prev&&usb>=p8_usb_prev?usb-p8_usb_prev:0;
    uint64_t touch_delta=touch_ok&&p8_touch_has_prev&&touch>=p8_touch_prev?
                         touch-p8_touch_prev:0;
    const bool uv=usb_ok&&p8_usb_has_prev&&usb>=p8_usb_prev;
    const bool tv=touch_ok&&p8_touch_has_prev&&touch>=p8_touch_prev;
    p8_usb_prev=usb;p8_touch_prev=touch;
    p8_usb_has_prev=usb_ok;p8_touch_has_prev=touch_ok;
    uint32_t on_mask=0,cfg_mask=0;
    if(pmu.valid)for(unsigned i=0;i<4;i++){
        /* The low nibble is the Exynos PMU per-core power bitfield.
         * 0xf is an ON register indication, not proof of execution. */
        if((pmu.cpu_status[i]&15u)==15u)on_mask|=1u<<i;
        if((pmu.cpu_config[i]&15u)==15u)cfg_mask|=1u<<i;
    }
    uint64_t uptime_ns=0;
    absolutetime_to_nanoseconds(now,&uptime_ns);
    const uint64_t head=__atomic_load_n(&p8_head,__ATOMIC_RELAXED);
    struct p8_row *r=&p8_rows[head%P8_RING];
    __atomic_store_n(&r->seq,0,__ATOMIC_RELEASE);
    r->up_ms=uptime_ns/NSEC_PER_MSEC;
    r->phase=__atomic_load_n(&p8_phase,__ATOMIC_RELAXED);
    r->flags=flags;
    int disp=hakios_display_requested_on();
    r->disp=disp<0?2u:(uint32_t)disp;
    r->lit_idle=lit_n?lit_sum/lit_n:P8_UNAVAILABLE;
    r->big_idle=big_n?big_sum/big_n:P8_UNAVAILABLE;
    r->lit_on=lit_on;r->big_on=big_on;
    r->exit_mask=hakios_cpu_exit_mask;
    r->lit_mhz=clusters[0].cur/1000;r->big_mhz=clusters[1].cur/1000;
    r->lit_cap_mhz=ceiling_now(&clusters[0])/1000;
    r->big_cap_mhz=ceiling_now(&clusters[1])/1000;
    r->big_mc=(flags&16u)?hakios_exynos_tmu_zone_mc(0):-273000;
    r->lit_mc=(flags&16u)?hakios_exynos_tmu_zone_mc(1):-273000;
    r->g3d_mc=(flags&16u)?hakios_exynos_tmu_zone_mc(2):-273000;
    r->battery_c=(flags&256u)?hakios_battery_temperature():-273;
    r->pmu_valid=pmu.valid;
    r->pmu_on_mask=on_mask;r->pmu_cfg_mask=cfg_mask;
    r->noncpu_status=pmu.noncpu_status;r->l2_status=pmu.l2_status;
    r->usb_valid=uv?1u:0u;r->touch_valid=tv?1u:0u;
    r->usb_delta=usb_delta;r->touch_delta=touch_delta;
    __atomic_store_n(&r->seq,head+1,__ATOMIC_RELEASE);
    __atomic_store_n(&p8_head,head+1,__ATOMIC_RELEASE);
    if(p8_count<P8_RING)p8_count++;
    if(mode==2)
        say("p8: up=%llums ph=%u disp=%u lit_on=%u big_on=%u "
            "idle=%u/%u temp_mc=%d/%d/%d cap=%u/%u pmu_valid=%u "
            "pmu_on=%#x pmu_cfg=%#x usb_valid=%u usb=%llu "
            "touch_valid=%u touch=%llu physical_off=unproven\n",
            (unsigned long long)r->up_ms,r->phase,r->disp,r->lit_on,r->big_on,
            r->lit_idle,r->big_idle,r->big_mc,r->lit_mc,r->g3d_mc,
            r->lit_cap_mhz,r->big_cap_mhz,r->pmu_valid,
            r->pmu_on_mask,r->pmu_cfg_mask,r->usb_valid,
            (unsigned long long)r->usb_delta,r->touch_valid,
            (unsigned long long)r->touch_delta);
}
#endif
