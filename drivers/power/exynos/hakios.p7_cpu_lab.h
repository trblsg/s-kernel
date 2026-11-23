/* P7 SoC base-power: 5s CPU idle/wakeup proxy black box.
 * Included after P4/P6 headers by exynos_cpufreq.c, on the EXISTING governor
 * thread. No extra timer, kernel thread or suspend operations. The ring is
 * intentionally readable after USB disconnect/reconnect.
 * Important: scheduler-idle is NOT WFI, C2, CPD or real wakeup residency.
 */
#ifndef HAKIOS_P7_CPU_LAB_H
#define HAKIOS_P7_CPU_LAB_H
#define P7_CPU_RING 96u
extern int hakios_display_requested_on(void);
struct p7_cpu_row {
    volatile uint64_t seq;
    uint64_t uptime_ms, usb_delta, usb_connected_delta, touch_poll_delta;
    uint32_t phase, lit_idle, big_idle, lit_online, big_online, lit_mhz,
             big_mhz, usb_valid, touch_valid, display_on, exit_mask;
};
static struct p7_cpu_row p7_cpu_rows[P7_CPU_RING];
static uint32_t p7_cpu_mode, p7_cpu_phase, p7_cpu_slot, p7_cpu_count;
static uint64_t p7_cpu_head, p7_cpu_prev_time, p7_cpu_prev_idle[8];
static uint64_t p7_cpu_prev_usb, p7_cpu_prev_connected, p7_cpu_prev_touch;
static bool p7_cpu_usb_valid, p7_cpu_touch_valid;
extern int hakios_p7_cpu_park(unsigned enable);
static uint32_t p7_cpu_park;
static int
p7_cpu_park_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p7_cpu_park,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>1)return EINVAL;
    if(value==1 && !__atomic_load_n(&p7_cpu_mode,__ATOMIC_ACQUIRE))
        return EBUSY; /* never park without durable CPU observation */
    err=hakios_p7_cpu_park((unsigned)value);
    if(err)return err;
    __atomic_store_n(&p7_cpu_park,(uint32_t)value,__ATOMIC_RELEASE);
    say("p7-cpu: runtime_park=%d (0 restore BIG, 1 screen-off BIG only) "
        "deep_C2_CPD_SICD_still_not_implemented=1\n",value);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_cpu_park,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_cpu_park_sysctl,"I","P7 opt-in BIG CPU_OFF with boot sleep_cores=1, default0");
static bool
p7_kernel_quad(const char *name,uint64_t *value)
{
    size_t length=sizeof(*value);
    return kernel_sysctlbyname(name,value,&length,NULL,0)==0 &&
           length==sizeof(*value);
}
static int
p7_cpu_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int value=(int)__atomic_load_n(&p7_cpu_mode,__ATOMIC_RELAXED);
    int err=sysctl_io_number(req,value,sizeof(value),&value,NULL);
    if(err || req->newptr==USER_ADDR_NULL)return err;
    if(value<0 || value>1)return EINVAL;
    if(value && !__atomic_load_n(&gov_running,__ATOMIC_ACQUIRE))return ENODEV;
    __atomic_store_n(&p7_cpu_mode,(uint32_t)value,__ATOMIC_RELEASE);
    __atomic_store_n(&p7_cpu_prev_time,0,__ATOMIC_RELEASE);
    p7_cpu_usb_valid=false;p7_cpu_touch_valid=false;
    if(value==0 && __atomic_load_n(&p7_cpu_park,__ATOMIC_ACQUIRE)){
        /* Disabling ring while BIG cores are parked would obscure recovery. */
        (void)hakios_p7_cpu_park(0);
        __atomic_store_n(&p7_cpu_park,0,__ATOMIC_RELEASE);
    }
    say("p7-cpu: mode=%d ring=%u sample_ms=5000 core_idle=scheduler_only "
        "deep_C2_CPD_SICD=unmeasured no_power_writes=1\n",value,P7_CPU_RING);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_cpu_mode,
    CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_cpu_mode_sysctl,"I","P7 read-only per-core idle and USB/touch delta ring");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p7_cpu_phase,
    CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,&p7_cpu_phase,0,
    "P7 CPU marker (0-99), does not alter power states");
SYSCTL_UINT(_kern_hakios,OID_AUTO,p7_cpu_slot,
    CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,&p7_cpu_slot,0,
    "P7 CPU sample back from latest: 0 now, 1 previous, up to ring-1");
static int
p7_cpu_stats_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[196];
    snprintf(line,sizeof(line),"mode=%u phase=%u samples=%u ring=%u "
        "head_seq=%llu slot=%u durable=kernel_memory_only deep=unmeasured",
        __atomic_load_n(&p7_cpu_mode,__ATOMIC_RELAXED),
        __atomic_load_n(&p7_cpu_phase,__ATOMIC_RELAXED),
        p7_cpu_count,P7_CPU_RING,(unsigned long long)p7_cpu_head,p7_cpu_slot);
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_cpu_stats,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_cpu_stats_sysctl,"A","P7 CPU ring status");
static int
p7_cpu_record_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    char line[250];
    const uint64_t head=__atomic_load_n(&p7_cpu_head,__ATOMIC_ACQUIRE);
    const uint32_t slot=__atomic_load_n(&p7_cpu_slot,__ATOMIC_RELAXED);
    if(!head || slot>=P7_CPU_RING || slot>=p7_cpu_count){
        snprintf(line,sizeof(line),"unavailable=1 head=%llu slot=%u samples=%u",
                 (unsigned long long)head,slot,p7_cpu_count);
    }else{
        struct p7_cpu_row *r=&p7_cpu_rows[(head-1-slot)%P7_CPU_RING];
        uint64_t before=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        uint64_t up=r->uptime_ms,usb=r->usb_delta,touch=r->touch_poll_delta;
        uint64_t connected=r->usb_connected_delta;
        uint32_t phase=r->phase,lit=r->lit_idle,big=r->big_idle;
        uint32_t lo=r->lit_online,bo=r->big_online,lmhz=r->lit_mhz,bmhz=r->big_mhz;
        uint32_t uv=r->usb_valid,tv=r->touch_valid,disp=r->display_on,mask=r->exit_mask;
        uint64_t after=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        if(!before || before!=after)
            snprintf(line,sizeof(line),"busy=1 head=%llu slot=%u",
                     (unsigned long long)head,slot);
        else
            snprintf(line,sizeof(line),"seq=%llu up_ms=%llu phase=%u disp=%u "
                "lit_idle=%u big_idle=%u lit_on=%u big_on=%u mhz=%u/%u "
                "exit=%#x usb_valid=%u usb_delta=%llu usb_connected=%llu "
                "touch_valid=%u touch_delta=%llu deep=unmeasured",
                (unsigned long long)before,(unsigned long long)up,
                phase,disp,lit,big,lo,bo,lmhz,bmhz,mask,
                uv,(unsigned long long)usb,(unsigned long long)connected,
                tv,(unsigned long long)touch);
    }
    return SYSCTL_OUT(req,line,strlen(line)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p7_cpu_record,
    CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p7_cpu_record_sysctl,"A","P7 latest-or-selected CPU/USB/touch ring row");

static void
p7_cpu_probe(uint64_t now)
{
    if(__atomic_load_n(&p7_cpu_mode,__ATOMIC_RELAXED)!=1)return;
    uint64_t prev=__atomic_load_n(&p7_cpu_prev_time,__ATOMIC_ACQUIRE),span=0;
    if(prev){
        if(now<=prev)return;
        absolutetime_to_nanoseconds(now-prev,&span);
        if(span<UINT64_C(5000000000))return;
    }
    const ml_topology_info_t *topology=ml_get_topology_info();
    uint32_t lit_sum=0,big_sum=0,lit_n=0,big_n=0,lit_on=0,big_on=0;
    for(unsigned i=0;i<8;i++){
        uint64_t idle=0;
        bool read_ok=i<topology->num_cpus && ml_cpu_idle_time((int)i,&idle);
        bool online=i>=32 || !(hakios_cpu_exit_mask&(1u<<i));
        uint32_t cluster=i<topology->num_cpus?
           (uint32_t)((topology->cpus[i].phys_id>>8)&0xff):0;
        bool big=cluster==clusters[1].affinity;
        if(online){if(big)big_on++;else lit_on++;}
        unsigned pct=s9_p4_idle_percent(p7_cpu_prev_idle[i],idle,
            prev?now-prev:0,prev && read_ok && idle &&
            p7_cpu_prev_idle[i],online);
        if(read_ok)p7_cpu_prev_idle[i]=idle;
        if(pct!=S9_P4_NO_SAMPLE){if(big){big_sum+=pct;big_n++;}
                                 else{lit_sum+=pct;lit_n++;}}
    }
    __atomic_store_n(&p7_cpu_prev_time,now,__ATOMIC_RELEASE);
    if(!prev)return;
    uint64_t usb=0,connected=0,touch=0;
    bool usb_ok=p7_kernel_quad("kern.hakios.p6_usb_ticks",&usb) &&
                p7_kernel_quad("kern.hakios.p6_usb_connected_ticks",&connected);
    bool touch_ok=p7_kernel_quad("kern.hakios.p6_touch_polls",&touch);
    uint64_t usb_delta=usb_ok && p7_cpu_usb_valid && usb>=p7_cpu_prev_usb?
                       usb-p7_cpu_prev_usb:0;
    uint64_t con_delta=usb_ok && p7_cpu_usb_valid && connected>=p7_cpu_prev_connected?
                       connected-p7_cpu_prev_connected:0;
    uint64_t touch_delta=touch_ok && p7_cpu_touch_valid && touch>=p7_cpu_prev_touch?
                         touch-p7_cpu_prev_touch:0;
    bool usb_delta_valid=usb_ok && p7_cpu_usb_valid && usb>=p7_cpu_prev_usb;
    bool touch_delta_valid=touch_ok && p7_cpu_touch_valid && touch>=p7_cpu_prev_touch;
    p7_cpu_prev_usb=usb;p7_cpu_prev_connected=connected;p7_cpu_prev_touch=touch;
    p7_cpu_usb_valid=usb_ok;p7_cpu_touch_valid=touch_ok;
    uint64_t uptime_ns=0;
    absolutetime_to_nanoseconds(now,&uptime_ns);
    uint64_t head=__atomic_load_n(&p7_cpu_head,__ATOMIC_RELAXED);
    struct p7_cpu_row *r=&p7_cpu_rows[head%P7_CPU_RING];
    __atomic_store_n(&r->seq,0,__ATOMIC_RELEASE);
    r->uptime_ms=uptime_ns/NSEC_PER_MSEC;
    r->phase=__atomic_load_n(&p7_cpu_phase,__ATOMIC_RELAXED);
    r->lit_idle=lit_n?lit_sum/lit_n:255;
    r->big_idle=big_n?big_sum/big_n:255;
    r->lit_online=lit_on;r->big_online=big_on;
    r->lit_mhz=clusters[0].cur/1000;r->big_mhz=clusters[1].cur/1000;
    int requested=hakios_display_requested_on();
    r->display_on=requested<0?2u:(uint32_t)requested;
    r->exit_mask=hakios_cpu_exit_mask;
    r->usb_valid=usb_delta_valid?1u:0u;
    r->touch_valid=touch_delta_valid?1u:0u;
    r->usb_delta=usb_delta;r->usb_connected_delta=con_delta;
    r->touch_poll_delta=touch_delta;
    __atomic_store_n(&r->seq,head+1,__ATOMIC_RELEASE);
    __atomic_store_n(&p7_cpu_head,head+1,__ATOMIC_RELEASE);
    if(p7_cpu_count<P7_CPU_RING)p7_cpu_count++;
    say("p7-cpu: up=%llums phase=%u disp=%u idle_lit=%u%% idle_big=%u%% "
        "lit_online=%u big_online=%u mhz=%u/%u exit=%#x "
        "usb_valid=%u usb_delta=%llu usb_connected=%llu "
        "touch_valid=%u touch_delta=%llu deep_C2_CPD_SICD=unmeasured\n",
        (unsigned long long)r->uptime_ms,r->phase,r->display_on,
        r->lit_idle,r->big_idle,r->lit_online,r->big_online,
        r->lit_mhz,r->big_mhz,r->exit_mask,r->usb_valid,
        (unsigned long long)usb_delta,(unsigned long long)con_delta,
        r->touch_valid,(unsigned long long)touch_delta);
}
#endif
