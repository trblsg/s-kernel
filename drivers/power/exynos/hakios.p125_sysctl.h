/* P12.5 debug-only observation controls, no hardware power policy writes. */
static int p125_page_select;
/* P12.6 policy is separate from the observation probe. All waits are bounded
 * so removal/expiry never depends on host USB or a producer wake. */
static unsigned
p125_thermal_reason(void)
{
    if(hakios_exynos_tmu_age_ms()>2000)return 3;
    int32_t b=hakios_exynos_tmu_zone_mc(0),l=hakios_exynos_tmu_zone_mc(1),g=hakios_exynos_tmu_zone_mc(2);
    if(b<0 || l<0 || g<0)return 3;
    int bat=hakios_battery_temperature();
    if(b>=76000 || l>=70000 || g>=72000 || bat>=47)return 2;
    if(hakios_display_requested_on()!=0)return 4;
    return 0;
}
static void
p126_guard(void)
{
    uint64_t lease=__atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE);
    if(!lease)return;
    unsigned reason=p125_thermal_reason();
    if(!reason && mach_absolute_time()>=(lease&~(uint64_t)P126_MASK))reason=1;
    if(reason)hakios_p126_stop(reason);
}
static int
p126_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p126_guard();
    int v=(int)hakios_p126_active(P126_MASK);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    if(v<0 || v>(int)P126_MASK)return EINVAL;
    if(!v){hakios_p126_stop(0);return 0;}
    if(__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE))return EBUSY;
    if(__atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) || hakios_p127_mode())return EBUSY;
    if(p125_thermal_reason())return EPERM;
    uint64_t span=0;
    nanoseconds_to_absolutetime(120ULL*NSEC_PER_SEC,&span);
    uint64_t expected=0,next=((mach_absolute_time()+span)&~(uint64_t)P126_MASK)|(unsigned)v;
    if(!__atomic_compare_exchange_n(&hakios_p126_lease,&expected,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    __atomic_store_n(&hakios_p126_reason,0,__ATOMIC_RELEASE);
    if(hakios_display_requested_on()!=0){hakios_p126_stop(4);return EPERM;}
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p126_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p126_mode_sysctl,"I","P12.6 OFF-only 120s lease: haptic1 wifi_worker2 gnss4 touch8 touch_leeway16 shub32");
static int
p126_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p126_guard();
    char buf[320];
    snprintf(buf,sizeof(buf),"version=1 mode=%u reason=%u haptic=%llu wifi_worker=%llu gnss=%llu touch=%llu touch_leeway=%llu shub=%llu lease_s=120 watchdog_ms=100",
        hakios_p126_active(P126_MASK),__atomic_load_n(&hakios_p126_reason,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[0],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[1],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[2],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[3],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[4],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p126_hits[5],__ATOMIC_RELAXED));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p126_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p126_status_sysctl,"A","P12.6 module application counters and stop reason");
static int
p126_diag_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    unsigned long long v[9];
    for(unsigned i=0;i<9;i++)v[i]=(unsigned long long)__atomic_load_n(&hakios_p126_diag[i],__ATOMIC_RELAXED);
    char buf[440];
    snprintf(buf,sizeof(buf),"version=1 bt_idle=%llu bt_busy=%llu bt_rx=%llu bt_tty_tx_dequeued=%llu bt_uart_error=%llu abox_quiet=%llu abox_busy=%llu shub_quiet=%llu shub_busy=%llu hal_speaker=%u",
        v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],hakios_p126_audio_get());
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p126_diag,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p126_diag_sysctl,"A","P12.6 read-only next-stage driver eligibility and traffic");
/* P12.7 420s nonrenewable trial; P126 is inherited only via bit0.
 * Stock P126 and P124 leases cannot coexist with this policy. */
static uint32_t
p127_floor_khz(unsigned mode)
{
    static const uint32_t rates[]={0,949000,832000,715000,598000,455000};
    unsigned i=P127_FLOOR(mode);
    return i<sizeof(rates)/sizeof(rates[0])?rates[i]:0;
}
/* P12.7.3: retain the exact refusal bitmap before the host can clean up.
 * Live eligibility is diagnostic only; it never grants a lease. */
static uint64_t p1273_floor_denied;
static unsigned
p1273_floor_reason(unsigned mode)
{
    uint32_t target=p127_floor_khz(mode);
    unsigned reason=0;
    uint64_t now=mach_absolute_time();
    if(!target)reason|=1u<<0;
    if(!p124_enabled || !gov_running || gov_policy!=2)reason|=1u<<1;
    if(__atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE))reason|=1u<<2;
    if(clusters[0].online!=4 || clusters[1].online!=4)reason|=1u<<3;
    if(hakios_cpu_exit_mask || big_cores!=SCION_BIG_CORES)reason|=1u<<4;
    if(clusters[0].floor!=1053000 || target<clusters[0].dt_min)reason|=1u<<5;
    if(!hakios_p124_ufs_safe())reason|=1u<<6;
    if(hakios_p124_ufs_busy())reason|=1u<<7;
    if(!__atomic_load_n(&touch_last,__ATOMIC_ACQUIRE))reason|=1u<<8;
    if(__atomic_load_n(&fingers,__ATOMIC_ACQUIRE))reason|=1u<<9;
    if(!hakios_p123_input_quiet(now))reason|=1u<<10;
    if(clusters[0].load>=40)reason|=1u<<11;
    if(clusters[0].thermal!=0)reason|=1u<<12;
    bool found=false;
    for(unsigned i=0;i<clusters[0].n;i++)if(clusters[0].levels[i]==target)found=true;
    if(!found)reason|=1u<<13;
    return reason;
}
static bool
p127_floor_safe(unsigned mode)
{
    return p1273_floor_reason(mode)==0;
}
static int
p1273_floor_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    uint64_t denied=__atomic_load_n(&p1273_floor_denied,__ATOMIC_ACQUIRE);
    unsigned mode=(unsigned)(denied>>32)&P127_MASK;
    if(!mode)mode=17;
    unsigned live=p1273_floor_reason(mode);
    char buf[320];
    snprintf(buf,sizeof(buf),"version=1 mode=%u denied=%u reason_mask=%x live_mask=%x input_seen=%u fingers=%u input_quiet=%u ufs_safe=%u ufs_busy=%u thermal=%u arm_thermal=%u",
        mode,denied?1u:0u,(unsigned)denied&0xffffffu,live,
        __atomic_load_n(&touch_last,__ATOMIC_ACQUIRE)?1u:0u,
        __atomic_load_n(&fingers,__ATOMIC_ACQUIRE),hakios_p123_input_quiet(mach_absolute_time())?1u:0u,
        hakios_p124_ufs_safe()?1u:0u,hakios_p124_ufs_busy()?1u:0u,
        p125_thermal_reason(),(unsigned)(denied>>24)&0xffu);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_floor,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p1273_floor_sysctl,"A","P12.7.3 LITTLE floor last denial and live guard bitmap");
/* P12.7.3 CPU accounting flight recorder; no change to governor decisions.
 * Active only after explicit boot opt-in and a bounded OFF-only request. */
static int p1273_trace_select;
/* One cached governor row per 250ms recorder bin. All fields atomic;
 * readers reject an in-flight publication. No extra timer or HW reads. */
struct p1273_gov_row {
    uint64_t seq,at,bin;
    uint32_t big_load,lit_load,big_khz,lit_khz,big_thermal,lit_thermal,tmu_age;
    int32_t big_mc,lit_mc,gpu_mc;
};
static struct p1273_gov_row p1273_gov_rows[128];
static unsigned p1273_gov_select;
static uint64_t p1273_gov_last_bin=UINT64_MAX;
static void
p1273_gov_record(uint64_t now)
{
    uint64_t start=__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_ACQUIRE);
    uint64_t span=__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_ACQUIRE);
    if(!span || now<start || now>=__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_ACQUIRE))return;
    uint64_t bin=(now-start)/span;
    if(bin==p1273_gov_last_bin)return;
    p1273_gov_last_bin=bin;
    struct p1273_gov_row *r=&p1273_gov_rows[bin%128];
    __atomic_fetch_add(&r->seq,1,__ATOMIC_SEQ_CST);
#define P1273_GOV_SET(k,v) __atomic_store_n(&r->k,(v),__ATOMIC_SEQ_CST)
    P1273_GOV_SET(at,now);P1273_GOV_SET(bin,bin);
    P1273_GOV_SET(big_load,clusters[1].load);P1273_GOV_SET(lit_load,clusters[0].load);
    P1273_GOV_SET(big_khz,clusters[1].cur);P1273_GOV_SET(lit_khz,clusters[0].cur);
    P1273_GOV_SET(big_thermal,clusters[1].thermal);P1273_GOV_SET(lit_thermal,clusters[0].thermal);
    P1273_GOV_SET(tmu_age,hakios_exynos_tmu_age_ms());
    P1273_GOV_SET(big_mc,hakios_exynos_tmu_zone_mc(0));
    P1273_GOV_SET(lit_mc,hakios_exynos_tmu_zone_mc(1));
    P1273_GOV_SET(gpu_mc,hakios_exynos_tmu_zone_mc(2));
#undef P1273_GOV_SET
    __atomic_fetch_add(&r->seq,1,__ATOMIC_SEQ_CST);
}
static int
p1273_gov_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    int v=(int)__atomic_load_n(&p1273_gov_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v<0 || v>=128)return EINVAL;
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=4)return EBUSY;
    __atomic_store_n(&p1273_gov_select,(unsigned)v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gov_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gov_select_sysctl,"I","P12.7.3 governor trace slot");
static int
p1273_gov_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=4)return EBUSY;
    unsigned slot=__atomic_load_n(&p1273_gov_select,__ATOMIC_ACQUIRE);
    struct p1273_gov_row *r=&p1273_gov_rows[slot],v={0};
    uint64_t seq=__atomic_load_n(&r->seq,__ATOMIC_SEQ_CST);
    if(seq&1)return EBUSY;
#define P1273_GOV_GET(k) v.k=__atomic_load_n(&r->k,__ATOMIC_SEQ_CST)
    P1273_GOV_GET(at);P1273_GOV_GET(bin);
    P1273_GOV_GET(big_load);P1273_GOV_GET(lit_load);
    P1273_GOV_GET(big_khz);P1273_GOV_GET(lit_khz);
    P1273_GOV_GET(big_thermal);P1273_GOV_GET(lit_thermal);
    P1273_GOV_GET(tmu_age);P1273_GOV_GET(big_mc);P1273_GOV_GET(lit_mc);P1273_GOV_GET(gpu_mc);
#undef P1273_GOV_GET
    if(seq!=__atomic_load_n(&r->seq,__ATOMIC_SEQ_CST))return EBUSY;
    uint64_t end=__atomic_load_n(&hakios_p1273_trace_end,__ATOMIC_ACQUIRE);
    uint64_t start=__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_ACQUIRE);
    uint64_t span=__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_ACQUIRE);
    uint64_t last=span && end>start?(end-1-start)/span:0;
    unsigned present=seq && span && v.at>=start && v.at<end && v.bin<=last && last-v.bin<128;
    char buf[320];
    snprintf(buf,sizeof(buf),"version=1 slot=%u present=%u bin=%llu at=%llu big_load=%u lit_load=%u big_khz=%u lit_khz=%u big_thermal=%u lit_thermal=%u tmu_age=%u big_mc=%d lit_mc=%d gpu_mc=%d",
        slot,present,(unsigned long long)v.bin,(unsigned long long)v.at,v.big_load,v.lit_load,v.big_khz,v.lit_khz,v.big_thermal,v.lit_thermal,v.tmu_age,v.big_mc,v.lit_mc,v.gpu_mc);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gov_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gov_page_sysctl,"A","P12.7.3 cached governor thermal row");
static void
p1273_trace_tick(uint64_t now)
{
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=2)return;
    unsigned reason=p125_thermal_reason();
    if(reason){hakios_p1273_trace_stop(reason,now);return;}
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE)){
        hakios_p1273_trace_stop(6,now);return;
    }
    uint64_t until=__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_RELAXED);
    uint64_t trigger=__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE);
    if(!trigger && (clusters[0].load>=60 || clusters[1].load>=60)){
        uint64_t zero=0;
        (void)__atomic_compare_exchange_n(&hakios_p1273_trace_trigger,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        trigger=__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE);
    }
    uint64_t post=0;
    nanoseconds_to_absolutetime(10ULL*NSEC_PER_SEC,&post);
    if(trigger && trigger+post<until){
        until=trigger+post;
        __atomic_store_n(&hakios_p1273_trace_until,until,__ATOMIC_RELEASE);
    }
    if(now>=until)hakios_p1273_trace_stop(trigger && until==trigger+post?7:1,until);
    else p1273_gov_record(now);
}
static int
p1273_trace_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    int v=__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)==2?1:0;
    int error=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(error || req->newptr==USER_ADDR_NULL)return error;
    if(v!=0 && v!=1)return EINVAL;
    uint64_t now=mach_absolute_time();
    if(__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE))return EBUSY;
    if(!v){hakios_p1273_trace_stop(0,now);return 0;}
    if(__atomic_load_n(&hakios_p1274_trace_late,__ATOMIC_ACQUIRE))return EBUSY;
    int enabled=0;
    if(!PE_parse_boot_argn("p1273",&enabled,sizeof(enabled)) || enabled!=1 ||
       !gov_running || p125_thermal_reason())return EPERM;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE))return EBUSY;
    uint64_t span=0,bucket=0;
    nanoseconds_to_absolutetime(180ULL*NSEC_PER_SEC,&span);
    nanoseconds_to_absolutetime(250ULL*NSEC_PER_MSEC,&bucket);
    if(!hakios_p1273_trace_arm(now,now+span,bucket))return EBUSY;
    unsigned reason=p125_thermal_reason();
    if(reason){hakios_p1273_trace_stop(reason,mach_absolute_time());return EPERM;}
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_trace_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p1273_trace_mode_sysctl,"I","P12.7.3 explicit 180s OFF recount trace, 0 freeze");
static int
p1273_trace_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[320];
    snprintf(buf,sizeof(buf),"version=1 state=%u generation=%llu start=%llu end=%llu until=%llu trigger=%llu span=%llu reason=%u buckets=128 owners=16",
        __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_end,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hakios_p1273_trace_reason,__ATOMIC_ACQUIRE));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_trace_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p1273_trace_status_sysctl,"A","P12.7.3 trace state, monotonic ticks and capture bounds");
static int
p1273_trace_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    int v=__atomic_load_n(&p1273_trace_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>=8*128*8)return EINVAL;
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=4)return EBUSY;
    __atomic_store_n(&p1273_trace_select,v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_trace_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p1273_trace_select_sysctl,"I","P12.7.3 frozen page select cpu*1024+slot*8+page");
static bool
p1273_trace_present(const struct p1273_trace_bucket *b)
{
    uint64_t gen=__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_RELAXED);
    uint64_t start=__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_RELAXED);
    uint64_t end=__atomic_load_n(&hakios_p1273_trace_end,__ATOMIC_RELAXED);
    uint64_t span=__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_RELAXED);
    if(!span || end<=start || b->generation!=gen)return false;
    uint64_t last=(end-1-start)/span;
    return b->bin<=last && last-b->bin<P1273_TRACE_BUCKETS && b->mask;
}
static int
p1273_trace_cpu_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int cpu,struct sysctl_req *req)
{
    if(cpu<0 || cpu>=8)return EINVAL;
    if(!hakios_p1273_trace_read_begin())return EBUSY;
    uint64_t masks[2]={0,0};
    for(unsigned i=0;i<P1273_TRACE_BUCKETS;i++){
        if(p1273_trace_present(&hakios_p1273_trace_rows[cpu][i]))masks[i/64]|=1ULL<<(i%64);
    }
    char buf[240];
    snprintf(buf,sizeof(buf),"version=1 cpu=%d generation=%llu mask=%llx/%llx overhead=%llu clipped=%llu",
        cpu,(unsigned long long)__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_RELAXED),
        (unsigned long long)masks[0],(unsigned long long)masks[1],
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_overhead[cpu],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p1273_trace_clipped[cpu],__ATOMIC_RELAXED));
    hakios_p1273_trace_read_end();
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
#define P1273_TRACE_CPU(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_trace_cpu##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p1273_trace_cpu_sysctl,"A","P12.7.3 frozen ring coverage")
P1273_TRACE_CPU(0); P1273_TRACE_CPU(1); P1273_TRACE_CPU(2); P1273_TRACE_CPU(3);
P1273_TRACE_CPU(4); P1273_TRACE_CPU(5); P1273_TRACE_CPU(6); P1273_TRACE_CPU(7);
#undef P1273_TRACE_CPU
static int
p1273_trace_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    if(!hakios_p1273_trace_read_begin())return EBUSY;
    unsigned select=(unsigned)__atomic_load_n(&p1273_trace_select,__ATOMIC_ACQUIRE);
    unsigned cpu=select/1024,slot=(select/8)%128,page=select%8;
    const struct p1273_trace_bucket *b=&hakios_p1273_trace_rows[cpu][slot];
    bool present=p1273_trace_present(b);
    char buf[448];int n=snprintf(buf,sizeof(buf),
        "version=1 select=%u generation=%llu present=%u bin=%llu mask=%x dropped=%u dropped_ticks=%llu",
        select,(unsigned long long)__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_RELAXED),
        present?1u:0u,(unsigned long long)(present?b->bin:0),present?b->mask:0,
        present?b->dropped:0,(unsigned long long)(present?b->dropped_ticks:0));
    if(n<0 || (size_t)n>=sizeof(buf)){hakios_p1273_trace_read_end();return EOVERFLOW;}
    size_t at=(size_t)n;
    if(present)for(unsigned i=page*2;i<page*2+2;i++){
        if(!(b->mask&(1u<<i)))continue;
        const struct p1273_trace_owner *o=&b->owner[i];
        n=snprintf(buf+at,sizeof(buf)-at," r=%llu/%llu/%u/%u/%llu/%llu/%u/%u",
            (unsigned long long)o->tid,(unsigned long long)o->task,o->pid,o->kind,
            (unsigned long long)o->user_ticks,(unsigned long long)o->system_ticks,o->fragments,o->switches);
        if(n<0 || (size_t)n>=sizeof(buf)-at){hakios_p1273_trace_read_end();return EOVERFLOW;}
        at+=(size_t)n;
    }
    hakios_p1273_trace_read_end();
    return SYSCTL_OUT(req,buf,at+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_trace_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p1273_trace_page_sysctl,"A","P12.7.3 frozen PID/task/TID recount rows");
/* P12.7.3 OFF burst cap: independent, nonrenewable waiting/active
 * experiment. Mode 3 is observation-only, same trigger and duration.
 * All ceiling changes are lower-only and checked on each caller. */
static LCK_GRP_DECLARE(p1273_cap_group,"p1273-cap");
static LCK_SPIN_DECLARE(p1273_cap_lock,&p1273_cap_group);
static uint32_t p1273_cap_state,p1273_cap_request,p1273_cap_reason;
static uint64_t p1273_cap_generation,p1273_cap_wait_until,p1273_cap_high_since;
static uint64_t p1273_cap_start,p1273_cap_end,p1273_cap_until;
static uint64_t p1273_cap_word; /* active absolute deadline | request (low 2 bits) */
static uint32_t p1273_cap_big_peak,p1273_cap_lit_peak,p1273_cap_gpu_peak;
static uint32_t p1273_cap_big_max_seen,p1273_cap_lit_max_seen,p1273_cap_big_asked,p1273_cap_lit_asked;
static uint64_t p1273_cap_windows,p1273_cap_big_hits,p1273_cap_lit_hits;
static uint32_t p1273_cap_mild_complete;
static uint32_t p1273_cap_measured_ok,p1273_cap_measured_bad;
static uint32_t p1273_cap_target(unsigned mode,unsigned cluster)
{
    if(mode==1)return cluster?1469000:1248000;
    if(mode==2)return cluster?1261000:1053000;
    return 0;
}
static bool p1273_cap_rate_present(struct cluster *c,uint32_t target)
{
    if(!target || target<c->dt_min || target>c->dt_max)return false;
    for(unsigned i=0;i<c->n;i++)if(c->levels[i]==target)return true;
    return false;
}
static unsigned p1273_cap_guard(uint64_t now)
{
    unsigned reason=p125_thermal_reason();
    if(reason)return reason;
    if(__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE))return 6;
    if(!gov_running || gov_policy!=2 || clusters[0].online!=4 || clusters[1].online!=4 ||
        hakios_cpu_exit_mask || big_cores!=SCION_BIG_CORES ||
        __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE))return 6;
    if(!hakios_p123_input_quiet(now))return 7;
    if(!hakios_p124_ufs_safe() || hakios_p124_ufs_busy())return 8;
    if(clusters[0].thermal || clusters[1].thermal)return 9;
    if(hakios_battery_temperature()<=0)return 3;
    return 0;
}
static void p1273_cap_finish_locked(unsigned reason,uint64_t now)
{
    if(p1273_cap_state!=2 && p1273_cap_state!=3)return;
    __atomic_store_n(&p1273_cap_word,0,__ATOMIC_RELEASE);
    if(p1273_cap_state==3 && reason==1 && p1273_cap_request==1 &&
       p1273_cap_windows && p1273_cap_big_hits && p1273_cap_lit_hits &&
       p1273_cap_measured_ok>=2 && !p1273_cap_measured_bad)p1273_cap_mild_complete=1;
    p1273_cap_end=now;p1273_cap_reason=reason;__atomic_store_n(&p1273_cap_state,4,__ATOMIC_RELEASE);
}
static void p1273_cap_tick(uint64_t now)
{
    /* Keep all guard work outside the spin lock. No new periodic worker. */
    if(!__atomic_load_n(&p1273_cap_word,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&p1273_cap_state,__ATOMIC_ACQUIRE)!=2)return;
    unsigned reason=p1273_cap_guard(now);
    uint64_t high_span=0,active_span=0;
    nanoseconds_to_absolutetime(250ULL*NSEC_PER_MSEC,&high_span);
    nanoseconds_to_absolutetime(15ULL*NSEC_PER_SEC,&active_span);
    int32_t b=hakios_exynos_tmu_zone_mc(0),l=hakios_exynos_tmu_zone_mc(1),g=hakios_exynos_tmu_zone_mc(2);
    lck_spin_lock(&p1273_cap_lock);
    if(p1273_cap_state!=2 && p1273_cap_state!=3)goto out;
    if(reason){p1273_cap_finish_locked(reason,now);goto out;}
    if(p1273_cap_state==2){
        if(now>=p1273_cap_wait_until){p1273_cap_finish_locked(10,now);goto out;}
        if(clusters[0].load<60 && clusters[1].load<60){p1273_cap_high_since=0;goto out;}
        if(!p1273_cap_high_since){p1273_cap_high_since=now;goto out;}
        if(now-p1273_cap_high_since<high_span)goto out;
        p1273_cap_start=now;p1273_cap_until=(now+active_span)&~3ULL;
        __atomic_store_n(&p1273_cap_state,3,__ATOMIC_RELEASE);
        __atomic_store_n(&p1273_cap_word,p1273_cap_until|p1273_cap_request,__ATOMIC_RELEASE);
    }
    if(now>=p1273_cap_until){p1273_cap_finish_locked(1,p1273_cap_until);goto out;}
    p1273_cap_windows++;
    if(b>0 && (uint32_t)b>p1273_cap_big_peak)p1273_cap_big_peak=(uint32_t)b;
    if(l>0 && (uint32_t)l>p1273_cap_lit_peak)p1273_cap_lit_peak=(uint32_t)l;
    if(g>0 && (uint32_t)g>p1273_cap_gpu_peak)p1273_cap_gpu_peak=(uint32_t)g;
    if(clusters[1].cur>p1273_cap_big_max_seen)p1273_cap_big_max_seen=clusters[1].cur;
    if(clusters[0].cur>p1273_cap_lit_max_seen)p1273_cap_lit_max_seen=clusters[0].cur;
    p1273_cap_big_asked|=clusters[1].asked;p1273_cap_lit_asked|=clusters[0].asked;
    if(p1273_cap_request<3 && now>p1273_cap_start){
        if(clusters[1].cur<=p1273_cap_target(p1273_cap_request,1))p1273_cap_big_hits++;
        if(clusters[0].cur<=p1273_cap_target(p1273_cap_request,0))p1273_cap_lit_hits++;
    }
out:
    lck_spin_unlock(&p1273_cap_lock);
}
static uint32_t p1273_cap_ceiling(struct cluster *c,uint32_t stock)
{
    uint64_t word=__atomic_load_n(&p1273_cap_word,__ATOMIC_ACQUIRE);
    unsigned mode=(unsigned)(word&3u);
    if(mode!=1 && mode!=2)return stock;
    uint64_t now=mach_absolute_time();
    if(now>=(word&~3ULL) || hakios_display_requested_on()!=0 ||
        !hakios_p123_input_quiet(now) || hakios_exynos_tmu_age_ms()>2000 ||
        clusters[0].online!=4 || clusters[1].online!=4 || hakios_cpu_exit_mask ||
        gov_policy!=2 || big_cores!=SCION_BIG_CORES ||
        __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
        !hakios_p124_ufs_safe() || hakios_p124_ufs_busy())return stock;
    /* Never raise a stock/thermal/OCP ceiling. */
    uint32_t target=p1273_cap_target(mode,c==&clusters[1]);
    return MIN(stock,target);
}
static int p1273_cap_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    lck_spin_lock(&p1273_cap_lock);
    int v=(p1273_cap_state==2 || p1273_cap_state==3)?(int)p1273_cap_request:0;
    lck_spin_unlock(&p1273_cap_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v<0 || v>3)return EINVAL;
    uint64_t now=mach_absolute_time();
    if(!v){
        lck_spin_lock(&p1273_cap_lock);
        p1273_cap_finish_locked(0,now);
        lck_spin_unlock(&p1273_cap_lock);
        return 0;
    }
    int enabled=0;
    if(!PE_parse_boot_argn("p1273",&enabled,sizeof(enabled)) || enabled!=1 ||
       __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=4 ||
       !__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE) ||
       p1273_cap_guard(now))return EPERM;
    if(v!=3 && (!p1273_cap_rate_present(&clusters[0],p1273_cap_target(v,0)) ||
        !p1273_cap_rate_present(&clusters[1],p1273_cap_target(v,1))))return EPERM;
    uint64_t wait=0;nanoseconds_to_absolutetime(60ULL*NSEC_PER_SEC,&wait);
    lck_spin_lock(&p1273_cap_lock);
    if(p1273_cap_state==2 || p1273_cap_state==3){lck_spin_unlock(&p1273_cap_lock);return EBUSY;}
    if(v==2 && !p1273_cap_mild_complete){lck_spin_unlock(&p1273_cap_lock);return EPERM;}
    p1273_cap_generation++;p1273_cap_request=v;p1273_cap_reason=0;
    p1273_cap_wait_until=now+wait;p1273_cap_high_since=0;
    p1273_cap_start=p1273_cap_end=p1273_cap_until=0;
    p1273_cap_big_peak=p1273_cap_lit_peak=p1273_cap_gpu_peak=0;
    p1273_cap_big_max_seen=p1273_cap_lit_max_seen=p1273_cap_big_asked=p1273_cap_lit_asked=0;
    p1273_cap_windows=p1273_cap_big_hits=p1273_cap_lit_hits=0;
    p1273_cap_measured_ok=p1273_cap_measured_bad=0;
    if(v==1)p1273_cap_mild_complete=0;
    __atomic_store_n(&p1273_cap_state,2,__ATOMIC_RELEASE);
    lck_spin_unlock(&p1273_cap_lock);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_cap_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_cap_mode_sysctl,"I","P12.7.3 OFF burst 0 stop, 1 mild, 2 stronger, 3 observe");
static int p1273_cap_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[448];
    lck_spin_lock(&p1273_cap_lock);
    snprintf(buf,sizeof(buf),"version=1 state=%u mode=%u generation=%llu reason=%u start=%llu end=%llu until=%llu windows=%llu big_cap=%u lit_cap=%u big_hits=%llu lit_hits=%llu big_peak=%u lit_peak=%u gpu_peak=%u big_max=%u lit_max=%u big_asked=%u lit_asked=%u measured_ok=%u measured_bad=%u mild_done=%u",
        p1273_cap_state,p1273_cap_request,(unsigned long long)p1273_cap_generation,p1273_cap_reason,
        (unsigned long long)p1273_cap_start,(unsigned long long)p1273_cap_end,(unsigned long long)p1273_cap_until,
        (unsigned long long)p1273_cap_windows,p1273_cap_target(p1273_cap_request,1),p1273_cap_target(p1273_cap_request,0),
        (unsigned long long)p1273_cap_big_hits,(unsigned long long)p1273_cap_lit_hits,
        p1273_cap_big_peak,p1273_cap_lit_peak,p1273_cap_gpu_peak,p1273_cap_big_max_seen,p1273_cap_lit_max_seen,
        p1273_cap_big_asked,p1273_cap_lit_asked,p1273_cap_measured_ok,p1273_cap_measured_bad,p1273_cap_mild_complete);
    lck_spin_unlock(&p1273_cap_lock);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_cap_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_cap_status_sysctl,"A","P12.7.3 OFF cap trial status");
/* Use the existing independent frequency measurement, not the ACPM request
 * cache (FREQ_GET may return zero). No new measurement timer. */
static bool p1273_rate_matches(uint32_t measured_mhz,uint32_t requested_khz)
{
    uint64_t actual=(uint64_t)measured_mhz*1000;
    return actual && requested_khz && actual*100>=97ULL*requested_khz &&
        actual*100<=103ULL*requested_khz;
}
static uint64_t p12754_rate_at;
static uint32_t p12754_rate_cur,p12754_rate_meas;
static void p1273_measured_sample(uint64_t now)
{
    struct cluster *l=&clusters[0],*b=&clusters[1];
    unsigned mode=hakios_p127_mode();
    if(l->online==4 && p1273_rate_matches(l->measured,l->cur)){
        if(P127_FLOOR(mode) && l->cur==p127_floor_khz(mode))
            hakios_p1273_event(P1273_FLOOR_READBACK,1);
        else if(!mode && l->cur>=1053000)
            hakios_p1273_event(P1273_FLOOR_STOCK,1);
    }
    lck_spin_lock(&p1273_cap_lock);
    p12754_rate_at=now;p12754_rate_cur=l->cur;p12754_rate_meas=l->measured;
    if(p1273_cap_state==3 && p1273_cap_request<3 && now<p1273_cap_until){
        uint32_t lc=p1273_cap_target(p1273_cap_request,0),bc=p1273_cap_target(p1273_cap_request,1);
        if(l->cur<=lc && b->cur<=bc && p1273_rate_matches(l->measured,l->cur) &&
            p1273_rate_matches(b->measured,b->cur))p1273_cap_measured_ok++;
        else p1273_cap_measured_bad++;
    }
    lck_spin_unlock(&p1273_cap_lock);
}
#include "hakios.p1273_gate.h"
/* FunctionalGate is not an attestation API for arbitrary masks. The
 * host may acknowledge a completed, exact same-boot lease only after
 * driver evidence and explicit post-restore functional verification.
 * No persistent token, no automatic enablement, no ON policy lease. */
static LCK_GRP_DECLARE(p1273_gate_group,"p1273-gate");
static LCK_SPIN_DECLARE(p1273_gate_lock,&p1273_gate_group);
struct p1273_gate_counts {
    uint64_t usb[5],abox[4],bt[6],wifi[5],ticker[3],floor[2];
    uint64_t events[2][P1273_EVENT_COUNT];
    uint64_t faults;
};
static struct p1273_gate_counts p1273_gate_pre,p1273_gate_post;
static unsigned p1273_gate_phase,p1273_gate_mode,p1273_gate_generation,p1273_gate_ack_generation;
static unsigned p1273_gate_qualified,p1273_gate_last_missing;
static uint64_t p1273_gate_expiry,p1273_gate_lease,p1273_gate_fault_mark;
static uint64_t p1273_gate_valid_until[13];
static bool p12754_optin(void){
    static int cached=-1;int enabled=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(enabled<0){int v=0,p=0;enabled=PE_parse_boot_argn("p12754",&v,sizeof(v)) && v==1 && PE_parse_boot_argn("p12753",&p,sizeof(p)) && p==1;__atomic_store_n(&cached,enabled,__ATOMIC_RELEASE);}
    return enabled!=0;
}
static uint64_t
p1273_gate_faults(void)
{
    return __atomic_load_n(&hakios_p1273_fault_seq,__ATOMIC_ACQUIRE)+
        __atomic_load_n(&hakios_p127_abox[3],__ATOMIC_ACQUIRE)+
        __atomic_load_n(&hakios_p127_bt[3],__ATOMIC_ACQUIRE)+
        __atomic_load_n(&hakios_p127_wifi[3],__ATOMIC_ACQUIRE);
}
static void
p1273_gate_snapshot(struct p1273_gate_counts *c)
{
#define P1273_COPY(field,source,n) for(unsigned i=0;i<(n);i++)c->field[i]=__atomic_load_n(&(source)[i],__ATOMIC_RELAXED)
    P1273_COPY(usb,hakios_p127_usb,5);
    P1273_COPY(abox,hakios_p127_abox,4);
    P1273_COPY(bt,hakios_p127_bt,6);
    P1273_COPY(wifi,hakios_p127_wifi,5);
    P1273_COPY(ticker,hakios_p127_ticker,3);
    P1273_COPY(floor,hakios_p127_floor,2);
    for(unsigned phase=0;phase<2;phase++)
        for(unsigned i=0;i<P1273_EVENT_COUNT;i++)
            c->events[phase][i]=__atomic_load_n(&hakios_p1273_events[phase][i],__ATOMIC_RELAXED);
#undef P1273_COPY
    c->faults=p1273_gate_faults();
}
static void
p1273_gate_clear_locked(void)
{
    __atomic_store_n(&hakios_p1273_gate_watch,0,__ATOMIC_RELEASE);
    p1273_gate_phase=0;p1273_gate_mode=0;p1273_gate_lease=0;p1273_gate_expiry=0;
}
static void
p1273_gate_refresh_locked(uint64_t now)
{
    if(p1273_gate_phase && now>=p1273_gate_expiry)p1273_gate_clear_locked();
    uint64_t faults=p1273_gate_faults();
    if(p1273_gate_qualified && faults!=p1273_gate_fault_mark)p1273_gate_qualified=0;
    for(unsigned i=0;i<13;i++)
        if(now>=p1273_gate_valid_until[i])p1273_gate_qualified&=~(1u<<i);
    /* Higher qualification cannot outlive its prerequisites. */
    if(!(p1273_gate_qualified&P1273_G_FLOOR))p1273_gate_qualified&=~(P12754_G_832|P12754_G_715|P12754_G_598);
    if(!(p1273_gate_qualified&P12754_G_832))p1273_gate_qualified&=~(P12754_G_715|P12754_G_598);
    if(!(p1273_gate_qualified&P12754_G_715))p1273_gate_qualified&=~P12754_G_598;
    if(!p1273_gate_combined(903,p1273_gate_qualified,true))
        p1273_gate_qualified&=~(P1273_G_C5|P1273_G_C6);
    if(!p1273_gate_combined(919,p1273_gate_qualified,true))
        p1273_gate_qualified&=~P1273_G_C6;
}
static bool
p1273_gate_enabled(void)
{
    int enabled=0;
    return PE_parse_boot_argn("p1273",&enabled,sizeof(enabled)) && enabled==1;
}
static bool
p1273_gate_idle(void)
{
    unsigned trace=__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE);
    unsigned cap=__atomic_load_n(&p1273_cap_state,__ATOMIC_ACQUIRE);
    return !__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) &&
        (trace==0 || trace==4) && cap!=2 && cap!=3;
}
static unsigned
p1273_gate_missing_locked(void)
{
    if(p1273_gate_phase!=3)return 512u;
    struct p1273_gate_counts c;
    p1273_gate_snapshot(&c);
    struct p1273_gate_evidence e={0};
#define D(f,i) (p1273_gate_post.f[i]-p1273_gate_pre.f[i])
#define A(i) (p1273_gate_post.events[0][i]-p1273_gate_pre.events[0][i])
#define R(i) (c.events[1][i]-p1273_gate_post.events[1][i])
    e.usb_quiet=D(usb,1);e.usb_down=A(P1273_USB_DOWN);e.usb_up=A(P1273_USB_UP);
    e.abox_stop=D(abox,0);e.abox_resume=D(abox,1);e.abox_wait=D(abox,2);
    e.audio_feed=R(P1273_AUDIO_FEED);e.audio_capture=R(P1273_AUDIO_CAPTURE);e.audio_dma=R(P1273_AUDIO_DMA);
    e.bt_attach=D(bt,0);e.bt_detach=D(bt,4);e.bt_irq=D(bt,1);
    e.bt_rx=A(P1273_BT_RX);e.bt_tx=A(P1273_BT_TX);e.bt_post_rx=R(P1273_BT_RX);e.bt_post_tx=R(P1273_BT_TX);
    e.wifi_attach=D(wifi,0);e.wifi_detach=D(wifi,4);e.wifi_handled=A(P1273_WIFI_HANDLED);
    e.wifi_rx=A(P1273_WIFI_RX);e.wifi_tx=A(P1273_WIFI_TX);
    e.wifi_post_rx=R(P1273_WIFI_RX);e.wifi_post_tx=R(P1273_WIFI_TX);
    e.ticker_wait=D(ticker,0);e.ticker_due=D(ticker,1);e.ticker_wake=D(ticker,2);
    e.ticker_cancel=A(P1273_TICK_CANCEL);
    e.floor_hits=D(floor,0);e.floor_readback=A(P1273_FLOOR_READBACK);
    e.floor_restored=R(P1273_FLOOR_STOCK)>0;
    e.fault_free=c.faults==p1273_gate_pre.faults;
#undef D
#undef A
#undef R
    return p1273_gate_missing(p1273_gate_mode,&e);
}
static int
p1273_gate_begin_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(mach_absolute_time());
    int v=(int)p1273_gate_mode;
    lck_spin_unlock(&p1273_gate_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    if(v && !p1273_gate_bit((unsigned)v))return EINVAL;
    bool enabled=p1273_gate_enabled(),idle=p1273_gate_idle();
    unsigned thermal=p125_thermal_reason();
    bool battery_ok=hakios_battery_temperature()>0;
    uint64_t now=mach_absolute_time(),span=0;
    nanoseconds_to_absolutetime(300ULL*NSEC_PER_SEC,&span);
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(now);
    if(!v){
        if(p1273_gate_lease && __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)==p1273_gate_lease)hakios_p127_stop(12);
        p1273_gate_clear_locked();
        lck_spin_unlock(&p1273_gate_lock);
        return 0;
    }
    if(!enabled || thermal || !battery_ok || !idle || !p1273_gate_idle()){
        lck_spin_unlock(&p1273_gate_lock);return EPERM;
    }
    if(p1273_gate_phase){lck_spin_unlock(&p1273_gate_lock);return EBUSY;}
    unsigned prior=p12754_floor_prereq((unsigned)v);
    if(prior && (!p12754_optin() || (p1273_gate_qualified&prior)!=prior)){lck_spin_unlock(&p1273_gate_lock);return EPERM;}
    if(p1273_gate_required(v) && !p1273_gate_combined(v,p1273_gate_qualified,enabled)){
        lck_spin_unlock(&p1273_gate_lock);return EPERM;
    }
    if(p1273_gate_generation>=99999999u){lck_spin_unlock(&p1273_gate_lock);return EOVERFLOW;}
    p1273_gate_generation++;
    p1273_gate_phase=1;p1273_gate_mode=v;p1273_gate_expiry=now+span;
    p1273_gate_last_missing=512;
    /* Requalification removes the old bit, including dependent combined bits. */
    p1273_gate_qualified&=~p1273_gate_bit(v);
    p1273_gate_refresh_locked(now);
    lck_spin_unlock(&p1273_gate_lock);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_begin,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gate_begin_sysctl,"I","P12.7.3 prepare exact functional gate, 0 discard pending");
static bool
p1273_gate_stop_ok_locked(void)
{
    if(__atomic_load_n(&hakios_p1273_last_stop_lease,__ATOMIC_ACQUIRE)!=p1273_gate_lease)return false;
    unsigned reason=__atomic_load_n(&hakios_p1273_last_stop_reason,__ATOMIC_ACQUIRE);
    /* LITTLE qualification must prove wake revokes the live floor lease. */
    return (p1273_gate_mode==17 || p12754_floor_prereq(p1273_gate_mode))?reason==4:reason==0;
}
static int
p1273_gate_review_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(mach_absolute_time());
    int v=p1273_gate_phase==3?(int)p1273_gate_generation:0;
    lck_spin_unlock(&p1273_gate_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(mach_absolute_time());
    if(v<=0 || (unsigned)v!=p1273_gate_generation || p1273_gate_phase!=2 ||
        !p1273_gate_idle() || !p1273_gate_stop_ok_locked()){
        lck_spin_unlock(&p1273_gate_lock);return EPERM;
    }
    p1273_gate_snapshot(&p1273_gate_post);
    p1273_gate_phase=3;
    lck_spin_unlock(&p1273_gate_lock);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_review,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gate_review_sysctl,"I","P12.7.3 verify stopped lease and start foreground evidence");
static int
p1273_gate_ack_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    lck_spin_lock(&p1273_gate_lock);
    int v=(int)p1273_gate_ack_generation;
    lck_spin_unlock(&p1273_gate_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    uint64_t now=mach_absolute_time(),span=0;
    nanoseconds_to_absolutetime(3600ULL*NSEC_PER_SEC,&span);
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(now);
    if(v<=0 || (unsigned)v!=p1273_gate_generation || p1273_gate_phase!=3 ||
        !p1273_gate_idle() || !p1273_gate_stop_ok_locked() ||
        (p12754_floor_prereq(p1273_gate_mode) && (p1273_gate_qualified&p12754_floor_prereq(p1273_gate_mode))!=p12754_floor_prereq(p1273_gate_mode)) ||
        (p1273_gate_required(p1273_gate_mode) && !p1273_gate_combined(p1273_gate_mode,p1273_gate_qualified,true))){
        lck_spin_unlock(&p1273_gate_lock);return EPERM;
    }
    p1273_gate_last_missing=p1273_gate_missing_locked();
    if(p1273_gate_last_missing){lck_spin_unlock(&p1273_gate_lock);return EPERM;}
    unsigned bit=p1273_gate_bit(p1273_gate_mode);
    for(unsigned i=0;i<13;i++)if(bit==(1u<<i))p1273_gate_valid_until[i]=now+span;
    p1273_gate_fault_mark=p1273_gate_pre.faults;
    p1273_gate_qualified|=bit;
    p1273_gate_ack_generation=p1273_gate_generation;
    p1273_gate_clear_locked();
    lck_spin_unlock(&p1273_gate_lock);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_ack,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gate_ack_sysctl,"I","P12.7.3 acknowledge reviewed functional evidence for current generation");
static int
p1273_gate_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[320];
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(mach_absolute_time());
    unsigned missing=p1273_gate_phase?p1273_gate_missing_locked():p1273_gate_last_missing;
    snprintf(buf,sizeof(buf),"version=1 phase=%u mode=%u generation=%u qualified=%u missing=%u lease=%llu expiry=%llu faults=%llu",
        p1273_gate_phase,p1273_gate_mode,p1273_gate_generation,p1273_gate_qualified,missing,
        (unsigned long long)p1273_gate_lease,(unsigned long long)p1273_gate_expiry,(unsigned long long)p1273_gate_faults());
    lck_spin_unlock(&p1273_gate_lock);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gate_status_sysctl,"A","P12.7.3 RAM-only functional qualification state");
static int
p1273_gate_events_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int arg2,struct sysctl_req *req)
{
    if(arg2<0 || arg2>1)return EINVAL;
    char buf[448];
    size_t at=(size_t)snprintf(buf,sizeof(buf),"version=1 phase=%d values=",arg2);
    for(unsigned i=0;i<P1273_EVENT_COUNT;i++){
        int n=snprintf(buf+at,sizeof(buf)-at,"%s%llx",i?",":"",
            (unsigned long long)__atomic_load_n(&hakios_p1273_events[arg2][i],__ATOMIC_RELAXED));
        if(n<0 || (size_t)n>=sizeof(buf)-at)return EOVERFLOW;
        at+=(size_t)n;
    }
    return SYSCTL_OUT(req,buf,at+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_active,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_gate_events_sysctl,"A","P12.7.3 active policy evidence");
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_gate_post,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,1,p1273_gate_events_sysctl,"A","P12.7.3 post-restore evidence");
static void
p127_guard(void)
{
    uint64_t lease=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
    if(!lease){
        if(__atomic_load_n(&hakios_p1273_gate_watch,__ATOMIC_ACQUIRE)){
            lck_spin_lock(&p1273_gate_lock);
            p1273_gate_refresh_locked(mach_absolute_time());
            lck_spin_unlock(&p1273_gate_lock);
        }
        return;
    }
    unsigned reason=p125_thermal_reason();
    if(!reason && __atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)){
        uint64_t deadline=__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE);
        uint64_t up=__atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE);
        if(up && (!deadline || up<deadline))reason=8;
        else if(deadline && mach_absolute_time()>=deadline)reason=10;
    }
    if(!reason && !hakios_p127_mode())reason=1;
    unsigned mode=hakios_p127_mode();
    if(!reason && p12754_optin() && P127_FLOOR(mode)>=2){
        lck_spin_lock(&p1273_gate_lock);p1273_gate_refresh_locked(mach_absolute_time());
        unsigned prior=p12754_floor_prereq(mode);
        bool allowed=prior && (p1273_gate_qualified&prior)==prior &&
            ((p1273_gate_phase==2 && p1273_gate_mode==mode && p1273_gate_lease==lease) ||
             (p1273_gate_qualified&p1273_gate_bit(mode)));
        lck_spin_unlock(&p1273_gate_lock);if(!allowed)reason=11;
    }
    if(!reason && p1273_gate_required(mode)){
        lck_spin_lock(&p1273_gate_lock);
        p1273_gate_refresh_locked(mach_absolute_time());
        bool allowed=p1273_gate_combined(mode,p1273_gate_qualified,true) &&
            ((p1273_gate_phase==2 && p1273_gate_lease==lease) ||
             (p1273_gate_qualified&p1273_gate_bit(mode)));
        lck_spin_unlock(&p1273_gate_lock);
        if(!allowed)reason=11;
    }
    /* Busy I/O/load bypasses the floor at each governor decision; a
     * topology or ownership change revokes the whole experiment. */
    if(!reason && p1273_gate_required(mode) && (hakios_cpu_exit_mask || big_cores!=SCION_BIG_CORES || clusters[0].online!=4 || clusters[1].online!=4 || !gov_running || gov_policy!=2 || __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) || __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE)))reason=6;
    if(!reason && P127_FLOOR(mode) && (!gov_running || gov_policy!=2 ||
       clusters[0].online!=4 || clusters[1].online!=4 ||
       hakios_cpu_exit_mask || !hakios_p124_ufs_safe()))reason=6;
    if(reason)hakios_p127_stop(reason);
}
static int
p127_arm_mode(int v)
{
    if(v<0 || v>(int)P127_MASK || P127_FLOOR(v)>5)return EINVAL;
    if(!v){hakios_p127_stop(0);return 0;}
    if(__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE))return EBUSY;
    if(!(v&P127_BASE))return EINVAL;
    /* Only exact combined masks can pass the same-boot gate. */
    unsigned extras=(unsigned)v&~(P127_BASE|0x70u);
    bool combined=p1273_gate_required((unsigned)v)!=0;
    if(!combined && ((extras && (extras&(extras-1))) || (extras && P127_FLOOR(v))))return EINVAL;
    bool enabled=p1273_gate_enabled();
    if(__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE))return EBUSY;
    unsigned thermal=p125_thermal_reason();
    unsigned floor_reason=P127_FLOOR(v)?p1273_floor_reason((unsigned)v):0;
    if(thermal || floor_reason){
        if(P127_FLOOR(v))__atomic_store_n(&p1273_floor_denied,
            ((uint64_t)(unsigned)v<<32)|((uint64_t)thermal<<24)|floor_reason,__ATOMIC_RELEASE);
        return EPERM;
    }
    uint64_t span=0,expected=0;
    nanoseconds_to_absolutetime(((v&(P127_ABOX|P127_PSR|P127_WIFI|P127_BT|P127_TICKER)) || P127_FLOOR(v)?120ULL:420ULL)*NSEC_PER_SEC,&span);
    const unsigned us[]={100,500,1000,2000,3500,10000,20000,50000};
    for(unsigned i=0;i<8;i++){
        uint64_t ticks=0;
        nanoseconds_to_absolutetime((uint64_t)us[i]*1000,&ticks);
        __atomic_store_n(&hakios_p127_bounds[i],ticks,__ATOMIC_RELAXED);
    }
    uint64_t now=mach_absolute_time(),next=((now+span)&~(uint64_t)P127_MASK)|(unsigned)v;
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(now);
    if(combined && (!gov_running || gov_policy!=2 || hakios_cpu_exit_mask || big_cores!=SCION_BIG_CORES ||
       clusters[0].online!=4 || clusters[1].online!=4 ||
       !p1273_gate_combined(v,p1273_gate_qualified,enabled) ||
       (p1273_gate_phase!=1 && !(p1273_gate_qualified&p1273_gate_bit(v))))){
        lck_spin_unlock(&p1273_gate_lock);return EPERM;
    }
    unsigned prior=p12754_floor_prereq((unsigned)v);
    if(p12754_optin() && P127_FLOOR(v)>=2 && (!prior || (p1273_gate_qualified&prior)!=prior ||
       (p1273_gate_phase!=1 && !(p1273_gate_qualified&p1273_gate_bit(v))))){lck_spin_unlock(&p1273_gate_lock);return EPERM;}
    if(p1273_gate_phase && (p1273_gate_phase!=1 || p1273_gate_mode!=(unsigned)v)){
        lck_spin_unlock(&p1273_gate_lock);return EBUSY;
    }
    if(!p1273_gate_idle()){lck_spin_unlock(&p1273_gate_lock);return EBUSY;}
    if(p1273_gate_phase==1){
        uint64_t gate_span=0;nanoseconds_to_absolutetime(120ULL*NSEC_PER_SEC,&gate_span);
        if(gate_span<span)next=((now+gate_span)&~(uint64_t)P127_MASK)|(unsigned)v;
        p1273_gate_snapshot(&p1273_gate_pre);
        __atomic_store_n(&hakios_p1273_gate_watch,(unsigned)v,__ATOMIC_RELEASE);
    }
    if(!__atomic_compare_exchange_n(&hakios_p127_lease,&expected,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        if(p1273_gate_phase==1)__atomic_store_n(&hakios_p1273_gate_watch,0,__ATOMIC_RELEASE);
        lck_spin_unlock(&p1273_gate_lock);return EBUSY;
    }
    if(p1273_gate_phase==1){p1273_gate_lease=next;p1273_gate_phase=2;}
    __atomic_store_n(&hakios_p127_reason,0,__ATOMIC_RELEASE);
    lck_spin_unlock(&p1273_gate_lock);
    if(hakios_display_requested_on()!=0){hakios_p127_stop(4);return EPERM;}
    return 0;
}
static int
p127_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p127_guard();
    int v=(int)hakios_p127_mode();
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    return p127_arm_mode(v);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_mode_sysctl,"I","P12.7 base1 USB2 ABOX4 PSR8 LITTLE selector bits4..6, WiFi INTx128 BT256 ticker512, bounded OFF lease");
/* Read-only ACPM requests are made only on the existing governor worker,
 * never in USB sysctl. Keep rates/errors cached, no bus DVFS writes. */
static uint32_t p127_bus_khz[6],p127_bus_valid;
static uint64_t p127_bus_at;
static const unsigned p1273_bus_idx[]={0,1,5,7,8,9,2,3};
static struct { uint64_t seq,at,latency; uint32_t raw; int error; } p1273_bus_rows[8];
static unsigned p1273_bus_next;
static uint64_t p1273_bus_last;
static void
p127_bus_poll(uint64_t now)
{
    if(!hakios_p127_mode() || !hakios_p125_active())return;
    uint64_t interval=0;nanoseconds_to_absolutetime(10ULL*NSEC_PER_SEC,&interval);
    if(p1273_bus_last && now-p1273_bus_last<interval)return;
    p1273_bus_last=now;
    unsigned i=p1273_bus_next++%8;
    uint32_t khz=0;
    uint64_t before=mach_absolute_time();
    int error=hakios_acpm_dvfs_get(p1273_bus_idx[i],&khz);
    uint64_t at=mach_absolute_time();
    /* Single governor writer, SC atomics for coherent row publication. */
    __atomic_fetch_add(&p1273_bus_rows[i].seq,1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&p1273_bus_rows[i].at,at,__ATOMIC_SEQ_CST);
    __atomic_store_n(&p1273_bus_rows[i].latency,at-before,__ATOMIC_SEQ_CST);
    __atomic_store_n(&p1273_bus_rows[i].raw,khz,__ATOMIC_SEQ_CST);
    __atomic_store_n(&p1273_bus_rows[i].error,error,__ATOMIC_SEQ_CST);
    __atomic_fetch_add(&p1273_bus_rows[i].seq,1,__ATOMIC_SEQ_CST);
    if(i<6){
        __atomic_store_n(&p127_bus_khz[i],khz,__ATOMIC_RELAXED);
        if(!error && khz)__atomic_fetch_or(&p127_bus_valid,1u<<i,__ATOMIC_RELEASE);
        else __atomic_fetch_and(&p127_bus_valid,~(1u<<i),__ATOMIC_RELEASE);
        __atomic_store_n(&p127_bus_at,at,__ATOMIC_RELEASE);
    }
}
static int
p1273_bus_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int i,struct sysctl_req *req)
{
    if(i<0 || i>=8)return EINVAL;
    uint64_t seq=__atomic_load_n(&p1273_bus_rows[i].seq,__ATOMIC_SEQ_CST);
    if(seq&1)return EBUSY;
    uint64_t at=__atomic_load_n(&p1273_bus_rows[i].at,__ATOMIC_SEQ_CST);
    uint64_t latency=__atomic_load_n(&p1273_bus_rows[i].latency,__ATOMIC_SEQ_CST);
    uint32_t raw=__atomic_load_n(&p1273_bus_rows[i].raw,__ATOMIC_SEQ_CST);
    int error=__atomic_load_n(&p1273_bus_rows[i].error,__ATOMIC_SEQ_CST);
    if(seq!=__atomic_load_n(&p1273_bus_rows[i].seq,__ATOMIC_SEQ_CST))return EBUSY;
    uint64_t now=mach_absolute_time(),age_ns=0,latency_ns=0;
    if(at && now>=at)absolutetime_to_nanoseconds(now-at,&age_ns);
    absolutetime_to_nanoseconds(latency,&latency_ns);
    char buf[256];
    snprintf(buf,sizeof(buf),"version=1 index=%u seq=%llu sampled=%u errno=%d raw=%u valid=%u zero=%u at=%llu age_ms=%llu latency_us=%llu",
        p1273_bus_idx[i],(unsigned long long)seq,seq?1u:0u,error,raw,
        seq && !error && raw?1u:0u,seq && !error && !raw?1u:0u,
        (unsigned long long)at,(unsigned long long)(age_ns/NSEC_PER_MSEC),(unsigned long long)(latency_ns/1000));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
#define P1273_BUS(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_bus##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p1273_bus_sysctl,"A","P12.7.3 ACPM cached domain response")
P1273_BUS(0); P1273_BUS(1); P1273_BUS(2); P1273_BUS(3); P1273_BUS(4); P1273_BUS(5); P1273_BUS(6); P1273_BUS(7);
#undef P1273_BUS
static int
p127_bus_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    uint32_t k[6];for(unsigned i=0;i<6;i++)k[i]=__atomic_load_n(&p127_bus_khz[i],__ATOMIC_RELAXED);
    char buf[240];snprintf(buf,sizeof(buf),"version=1 at=%llu valid=%x mif=%u int=%u intcam=%u cam=%u disp=%u aud=%u",
        (unsigned long long)__atomic_load_n(&p127_bus_at,__ATOMIC_ACQUIRE),
        __atomic_load_n(&p127_bus_valid,__ATOMIC_ACQUIRE),k[0],k[1],k[2],k[3],k[4],k[5]);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_bus,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_bus_sysctl,"A","P12.7 cached read-only ACPM rate evidence");
static int
p127_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p127_guard();
    char buf[320];unsigned mode=hakios_p127_mode();
    snprintf(buf,sizeof(buf),"version=1 mode=%u reason=%u lease_s=%u little_request=%u little_cur=%u ufs_safe=%u ufs_busy=%u floor_hits=%llu floor_bypass=%llu",
        mode,__atomic_load_n(&hakios_p127_reason,__ATOMIC_ACQUIRE),((mode&(P127_ABOX|P127_PSR|P127_WIFI|P127_BT|P127_TICKER)) || P127_FLOOR(mode))?120u:420u,p127_floor_khz(mode),clusters[0].cur,
        hakios_p124_ufs_safe()?1u:0u,hakios_p124_ufs_busy()?1u:0u,
        (unsigned long long)__atomic_load_n(&hakios_p127_floor[0],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_floor[1],__ATOMIC_RELAXED));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_status_sysctl,"A","P12.7 lease and floor status");
static int
p127_metrics_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    unsigned long long v[13];
    for(unsigned i=0;i<5;i++)v[i]=(unsigned long long)__atomic_load_n(&hakios_p127_usb[i],__ATOMIC_RELAXED);
    for(unsigned i=0;i<4;i++){v[5+i]=(unsigned long long)__atomic_load_n(&hakios_p127_abox[i],__ATOMIC_RELAXED);
        v[9+i]=(unsigned long long)__atomic_load_n(&hakios_p127_psr[i],__ATOMIC_RELAXED);}
    char buf[448];
    snprintf(buf,sizeof(buf),"version=1 usb_stock=%llu usb_8ms=%llu usb_linkdown=%llu usb_bypass=%llu usb_other=%llu abox_stop=%llu abox_resume=%llu abox_idle_wait=%llu abox_fault=%llu psr_mask=%llu psr_unmask=%llu psr_bypass=%llu psr_fault=%llu",
        v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7],v[8],v[9],v[10],v[11],v[12]);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_metrics,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_metrics_sysctl,"A","P12.7 cumulative actual driver applications");
static int
p127_ticker_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[160];snprintf(buf,sizeof(buf),"version=1 wait=%llu due=%llu wake=%llu",
        (unsigned long long)__atomic_load_n(&hakios_p127_ticker[0],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_ticker[1],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_ticker[2],__ATOMIC_RELAXED));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_ticker,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_ticker_sysctl,"A","P12.7 deadline ticker evidence");
static int
p127_bt_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[280];unsigned long long v[6];
    for(unsigned i=0;i<6;i++)v[i]=(unsigned long long)__atomic_load_n(&hakios_p127_bt[i],__ATOMIC_RELAXED);
    snprintf(buf,sizeof(buf),"version=1 attach=%llu irq=%llu watchdog=%llu fallback=%llu detach=%llu tx_wake=%llu",v[0],v[1],v[2],v[3],v[4],v[5]);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_bt,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_bt_sysctl,"A","P12.7 UART RX interrupt and fallback evidence");
static int
p127_wifi_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[260];unsigned long long v[5];
    for(unsigned i=0;i<5;i++)v[i]=(unsigned long long)__atomic_load_n(&hakios_p127_wifi[i],__ATOMIC_RELAXED);
    snprintf(buf,sizeof(buf),"version=1 attach=%llu irq=%llu watchdog=%llu fallback=%llu detach=%llu",v[0],v[1],v[2],v[3],v[4]);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_wifi,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_wifi_sysctl,"A","P12.7 WiFi INTx trial and watchdog evidence");
/* Cached gauge samples only: never perform I2C in USB console. */
static int
p127_fg_seq_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[80];
    snprintf(buf,sizeof(buf),"version=1 seq=%llu slots=64",(unsigned long long)__atomic_load_n(&hakios_p127_fg_seq,__ATOMIC_ACQUIRE));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_fg_seq,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_fg_seq_sysctl,"A","P12.7 cached fuel gauge sequence");
static int
p127_fg_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int page,struct sysctl_req *req)
{
    if(page<0 || page>=16)return EINVAL;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    char buf[448];int n=snprintf(buf,sizeof(buf),"version=1 page=%d",page);
    if(n<0 || (size_t)n>=sizeof(buf))return EOVERFLOW;
    size_t at=(size_t)n;
    for(unsigned i=0;i<4;i++){
        struct p127_fg_row *r=&hakios_p127_fg_rows[(unsigned)page*4+i];
        uint64_t seq=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        if(!seq)continue;
        n=snprintf(buf+at,sizeof(buf)-at," r=%llu/%llu/%d/%d/%u/%u/%u/%u",
            (unsigned long long)seq,(unsigned long long)r->ticks,r->ua,r->avg_ua,r->uv,r->external,r->charger,r->qh);
        if(n<0 || (size_t)n>=sizeof(buf)-at)return EOVERFLOW;
        if(seq!=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE))return EBUSY;
        at+=(size_t)n;
    }
    return SYSCTL_OUT(req,buf,at+1);
}
#define P127_FG(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_fg##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p127_fg_page_sysctl,"A","P12.7 cached battery gauge samples")
P127_FG(0); P127_FG(1); P127_FG(2); P127_FG(3); P127_FG(4); P127_FG(5); P127_FG(6); P127_FG(7);
P127_FG(8); P127_FG(9); P127_FG(10); P127_FG(11); P127_FG(12); P127_FG(13); P127_FG(14); P127_FG(15);
#undef P127_FG
static int
p127_wfi_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int cpu,struct sysctl_req *req)
{
    if(cpu<0 || cpu>=16)return EINVAL;
    bool extended=cpu>=8;
    cpu%=8;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    char buf[448];size_t at=0;
    int n=snprintf(buf,sizeof(buf),"version=%u cpu=%d sgi=%llx ppi=%llx spi=%llx bins=",extended?2u:1u,cpu,
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][0],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][1],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][2],__ATOMIC_RELAXED));
    if(n<0 || (size_t)n>=sizeof(buf))return EOVERFLOW;
    at=(size_t)n;
    for(unsigned i=0;i<(extended?9u:7u);i++){
        uint64_t count=__atomic_load_n(&hakios_p127_wfi_n[cpu][i],__ATOMIC_RELAXED);
        uint64_t ticks=__atomic_load_n(&hakios_p127_wfi_ticks[cpu][i],__ATOMIC_RELAXED);
        if(!extended && i==6)for(unsigned j=7;j<9;j++){
            count+=__atomic_load_n(&hakios_p127_wfi_n[cpu][j],__ATOMIC_RELAXED);
            ticks+=__atomic_load_n(&hakios_p127_wfi_ticks[cpu][j],__ATOMIC_RELAXED);
        }
        n=snprintf(buf+at,sizeof(buf)-at,"%s%llx/%llx",i?",":"",
            (unsigned long long)count,(unsigned long long)ticks);
        if(n<0 || (size_t)n>=sizeof(buf)-at)return EOVERFLOW;
        at+=(size_t)n;
    }
    return SYSCTL_OUT(req,buf,at+1);
}
#define P127_WFI(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_wfi##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p127_wfi_sysctl,"A","P12.7 WFI count/ticks bins")
P127_WFI(0); P127_WFI(1); P127_WFI(2); P127_WFI(3); P127_WFI(4); P127_WFI(5); P127_WFI(6); P127_WFI(7);
#undef P127_WFI
#define P1273_WFI(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_wfi##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,(N)+8,p127_wfi_sysctl,"A","P12.7.3 WFI nine count/ticks bins")
P1273_WFI(0); P1273_WFI(1); P1273_WFI(2); P1273_WFI(3); P1273_WFI(4); P1273_WFI(5); P1273_WFI(6); P1273_WFI(7);
#undef P1273_WFI
/* Fixed-duration offline measurement is configured only with both
 * leases idle. Legacy EnergyOffline remains the link crossover test. */
/* Random session identity, generated on first explicit query only.
 * Never persistent, never derived from a device serial or other identifier. */
static uint64_t p1273_boot_nonce;
static int p1273_boot_identity_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    uint64_t nonce=__atomic_load_n(&p1273_boot_nonce,__ATOMIC_ACQUIRE);
    if(!nonce){
        uint64_t fresh=0,expected=0;
        read_random(&fresh,sizeof(fresh));
        if(!fresh)return EAGAIN;
        if(!__atomic_compare_exchange_n(&p1273_boot_nonce,&expected,fresh,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))fresh=expected;
        nonce=fresh;
    }
    char buf[64];
    snprintf(buf,sizeof(buf),"version=1 nonce=%016llx",(unsigned long long)nonce);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_boot_identity,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_boot_identity_sysctl,"A","P12.7.3 boot-local capture identity");
static int p1273_offline_s_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    uint64_t tps=0;nanoseconds_to_absolutetime(NSEC_PER_SEC,&tps);
    int v=tps?(int)(__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE)/tps):0;
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    if(v!=0 && v!=60)return EINVAL; /* first hardware qualification only */
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE))return EBUSY;
    int enabled=0;
    if(v && (!PE_parse_boot_argn("p1273",&enabled,sizeof(enabled)) || enabled!=1))return EPERM;
    __atomic_store_n(&hakios_p1273_offline_duration,(uint64_t)v*tps,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_offline_s,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_offline_s_sysctl,"I","P12.7.3 fixed 60s offline configuration, 0 legacy");
static int p1273_offline_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req)
{
    char buf[256];
    snprintf(buf,sizeof(buf),"version=1 fixed=%u duration=%llu deadline=%llu policy_reason=%u capture_reason=%u",
        __atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hakios_p127_reason,__ATOMIC_ACQUIRE),
        __atomic_load_n(&hakios_p125_stop_reason,__ATOMIC_ACQUIRE));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1273_offline_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1273_offline_status_sysctl,"A","P12.7.3 fixed offline deadline and stop reasons");
static void
p125_finish(unsigned reason)
{
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if(!(v&3u))return;
    uint64_t now=mach_absolute_time(),end=v&~3ULL;
    uint64_t up=__atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE);
    bool fixed=__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)!=0;
    if(fixed){
        uint64_t deadline=__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE);
        if(reason==10 && deadline && now>=deadline)end=deadline;
        else if(reason==8 && up && up<=end)end=up;
        else {end=now<end?now:end;if(deadline && end>deadline)end=deadline;}
        if(reason==0)reason=6; /* manual stop is not planned completion */
    }else{
        if((v&3u)==3u && up && reason==0 && up<=end)end=up;
        else if(now>=end)reason=1;
        else end=now;
        if((v&3u)==3u && !__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE) && reason==0)reason=5;
    }
    if(__atomic_compare_exchange_n(&hakios_p125_lease,&v,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        __atomic_store_n(&hakios_p125_end,end,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_stop_reason,reason,__ATOMIC_RELEASE);
        if(fixed){
            hakios_p127_stop(reason);
            __atomic_store_n(&hakios_p1273_offline_fixed,0,__ATOMIC_RELEASE);
        }
    }
}
static void
p125_guard(void)
{
    uint64_t lease=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if(!lease)return;
    if(__atomic_load_n(&hakios_p125_display_events,__ATOMIC_ACQUIRE)){p125_finish(4);return;}
    unsigned reason=p125_thermal_reason();
    if(reason){p125_finish(reason);return;}
    /* Keep legacy hard guards; P12753 owns normal deadline finalization. */
    if(__atomic_load_n(&hakios_p12753_observe,__ATOMIC_ACQUIRE))return;
    if(__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE)){
        uint64_t now=mach_absolute_time();
        uint64_t deadline=__atomic_load_n(&hakios_p1273_offline_deadline,__ATOMIC_ACQUIRE);
        uint64_t up=__atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE);
        uint64_t policy=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
        unsigned policy_reason=__atomic_load_n(&hakios_p127_reason,__ATOMIC_ACQUIRE);
        if(up && (!deadline || up<deadline)){p125_finish(8);return;}
        if(!policy && policy_reason!=10){p125_finish(6);return;}
        if(deadline && now>=deadline){p125_finish(10);return;}
    }else if((lease&3u)==3u && __atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE)){p125_finish(0);return;}
    if(mach_absolute_time()>=(lease&~3ULL))p125_finish(1);
}
static int
p125_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p125_guard();
    int v=(int)(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE)&3u);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    if(v<0 || v>3)return EINVAL;
    if(!v){p125_finish(0);return 0;}
    if(__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE))return EBUSY;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    if(p125_thermal_reason())return EPERM;
    const unsigned us[]={100,500,1000,10000};
    for(unsigned i=0;i<4;i++){
        uint64_t ticks=0;
        nanoseconds_to_absolutetime((uint64_t)us[i]*1000,&ticks);
        __atomic_store_n(&hakios_p124_wfi_bounds[i],ticks,__ATOMIC_RELAXED);
    }
    uint64_t span=0,now=mach_absolute_time();
    unsigned p127=hakios_p127_mode();
    uint64_t lease=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
    nanoseconds_to_absolutetime((p127 && (v==1 || v==3)?360ULL:180ULL)*NSEC_PER_SEC,&span);
    if(p127 && (lease&~(uint64_t)P127_MASK)<=now+span){
        uint64_t end=lease&~(uint64_t)P127_MASK;
        if(end<=now)return EPERM;
        span=end-now;
    }
    uint64_t fixed=__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE);
    if(fixed){
        uint64_t setup=0;nanoseconds_to_absolutetime(30ULL*NSEC_PER_SEC,&setup);
        if(v!=3 || !p127 || span<fixed+setup)return EPERM;
    }
    __atomic_store_n(&hakios_p1273_offline_deadline,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p127_offline_aggregate,(p127 && v==3)?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_usb_down_at,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_usb_up_at,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_display_events,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_start,v==3?0:now,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_end,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_stop_reason,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p1273_offline_fixed,fixed?1u:0u,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_lease,((now+span)&~3ULL)|(unsigned)v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p125_mode_sysctl,"I","P12.5 0 stop 1 aggregate 2 detail 3 link-down detail; 180s read-only lease");
static int
p125_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p125_guard();
    uint64_t tps=0;
    nanoseconds_to_absolutetime(NSEC_PER_SEC,&tps);
    char buf[480];
    snprintf(buf,sizeof(buf),"version=1 active=%u armed=%u start=%llu end=%llu now=%llu hz=%llu reason=%u down=%llu up=%llu display_on=%llu anchor=%llx slots=512 rows=2 counters=28 kinds=15 cumulative=1 page_lookup=scalar",
        hakios_p125_active(),(unsigned)(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE)&3u),
        (unsigned long long)__atomic_load_n(&hakios_p125_start,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_end,__ATOMIC_ACQUIRE),
        (unsigned long long)mach_absolute_time(),(unsigned long long)tps,
        __atomic_load_n(&hakios_p125_stop_reason,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_display_events,__ATOMIC_ACQUIRE),
        (unsigned long long)hakios_p125_code((uintptr_t)hakios_p125_active));
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p125_status_sysctl,"A","P12.5 probe lease and frozen readback");
static int
p125_counts_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int cpu,struct sysctl_req *req)
{
    if(cpu<0 || cpu>=8)return EINVAL;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    char buf[512];
    int at=snprintf(buf,sizeof(buf),"version=1 cpu=%d counters=",cpu);
    for(unsigned i=0;i<P125_NCOUNT;i++){
        int n=snprintf(buf+at,sizeof(buf)-(size_t)at,"%s%llx",i?",":"",
            (unsigned long long)__atomic_load_n(&hakios_p125_counts[cpu][i],__ATOMIC_RELAXED));
        if(n<0 || n>=(int)sizeof(buf)-at)return EOVERFLOW;
        at+=n;
    }
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
#define P125_COUNTS(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_counts##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p125_counts_sysctl,"A","P12.5 cumulative counters hex")
P125_COUNTS(0);P125_COUNTS(1);P125_COUNTS(2);P125_COUNTS(3);
P125_COUNTS(4);P125_COUNTS(5);P125_COUNTS(6);P125_COUNTS(7);
#undef P125_COUNTS
static int
p125_coverage_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,int cpu,struct sysctl_req *req)
{
    if(cpu<0 || cpu>=8)return EINVAL;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    char buf[512];
    int at=snprintf(buf,sizeof(buf),"version=1 cpu=%d pages=%016llx,%016llx,%016llx,%016llx mapdrop=%llu bins=",
        cpu,(unsigned long long)__atomic_load_n(&hakios_p125_pages[cpu][0],__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_pages[cpu][1],__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_pages[cpu][2],__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_pages[cpu][3],__ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&hakios_p125_thread_map_drops,__ATOMIC_RELAXED));
    for(unsigned i=0;i<5;i++){
        int n=snprintf(buf+at,sizeof(buf)-(size_t)at,"%s%llx",i?",":"",
            (unsigned long long)__atomic_load_n(&hakios_p125_intervals[cpu][i],__ATOMIC_RELAXED));
        if(n<0 || n>=(int)sizeof(buf)-at)return EOVERFLOW;
        at+=n;
    }
    int n=snprintf(buf+at,sizeof(buf)-(size_t)at," totals=");
    if(n<0 || n>=(int)sizeof(buf)-at)return EOVERFLOW;
    at+=n;
    for(unsigned i=1;i<P125_KINDS;i++){
        n=snprintf(buf+at,sizeof(buf)-(size_t)at,"%s%llx/%llx",i>1?",":"",
            (unsigned long long)__atomic_load_n(&hakios_p125_kind_total[cpu][i],__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hakios_p125_kind_drop[cpu][i],__ATOMIC_RELAXED));
        if(n<0 || n>=(int)sizeof(buf)-at)return EOVERFLOW;
        at+=n;
    }
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
#define P125_COVERAGE(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_coverage##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p125_coverage_sysctl,"A","P12.5 page bitmap, requested deadline bins and per-kind coverage")
P125_COVERAGE(0);P125_COVERAGE(1);P125_COVERAGE(2);P125_COVERAGE(3);
P125_COVERAGE(4);P125_COVERAGE(5);P125_COVERAGE(6);P125_COVERAGE(7);
#undef P125_COVERAGE
static int
p125_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    int v=__atomic_load_n(&p125_page_select,__ATOMIC_RELAXED);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>=2048)return EINVAL;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    __atomic_store_n(&p125_page_select,v,__ATOMIC_RELAXED);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p125_select_sysctl,"I","P12.5 frozen page cursor only; no policy writes");
static int
p125_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    unsigned page=(unsigned)__atomic_load_n(&p125_page_select,__ATOMIC_RELAXED);
    if(page>=2048)return EINVAL;
    unsigned cpu=page/256,start=(page%256)*2;
    char buf[512];
    int at=snprintf(buf,sizeof(buf),"version=1 page=%u",page);
    for(unsigned i=start;i<start+2;i++){
        struct p125_row *r=&hakios_p125_rows[cpu][i];
        if(__atomic_load_n(&r->state,__ATOMIC_ACQUIRE)!=2)continue;
        /* P12.5.1 USB readback must never take a proc reference, wait
         * for exec, resolve a live task, allocate, or sleep. USB OUT
         * rearm/polling is blocked until this synchronous handler returns.
         * Scalar PID + task generation were captured at the source.
         * Keep the wire shape; '-' explicitly means name not resolved. */
        const char *name="-";
#define R125(F) ((unsigned long long)__atomic_load_n(&r->F,__ATOMIC_RELAXED))
        int n=snprintf(buf+at,sizeof(buf)-(size_t)at," r=%x/%x/%llx/%llx/%llx/%llx/%llx/%llx/%llx/%llx/%llx/%llx/%s",
            i,r->kind,(unsigned long long)r->func,(unsigned long long)r->owner,(unsigned long long)r->aux,
            R125(count),R125(short_n),R125(reqzero),R125(effzero),R125(flags_or),R125(sample_n),R125(wall_ticks),name);
#undef R125
        if(n<0 || n>=(int)sizeof(buf)-at)return EOVERFLOW;
        at+=n;
    }
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p125_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p125_page_sysctl,"A","P12.5 frozen rows, unslid symbols and scalar owners");
