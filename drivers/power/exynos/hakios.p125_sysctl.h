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
    if(v<0 || v>(int)P126_MASK)return EINVAL;
    if(!v){hakios_p126_stop(0);return 0;}
    if(__atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE))return EBUSY;
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
static void
p125_finish(unsigned reason)
{
    uint64_t v=__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE);
    if(!(v&3u))return;
    uint64_t now=mach_absolute_time(),end=v&~3ULL;
    uint64_t up=__atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE);
    if((v&3u)==3u && up && reason==0 && up<=end)end=up;
    else if(now>=end)reason=1;
    else end=now;
    if((v&3u)==3u && !__atomic_load_n(&hakios_p125_usb_down_at,__ATOMIC_ACQUIRE) && reason==0)reason=5;
    if(__atomic_compare_exchange_n(&hakios_p125_lease,&v,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        __atomic_store_n(&hakios_p125_end,end,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_stop_reason,reason,__ATOMIC_RELEASE);
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
    if((lease&3u)==3u && __atomic_load_n(&hakios_p125_usb_up_at,__ATOMIC_ACQUIRE)){p125_finish(0);return;}
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
    if(v<0 || v>3)return EINVAL;
    if(!v){p125_finish(0);return 0;}
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    if(p125_thermal_reason())return EPERM;
    const unsigned us[]={100,500,1000,10000};
    for(unsigned i=0;i<4;i++){
        uint64_t ticks=0;
        nanoseconds_to_absolutetime((uint64_t)us[i]*1000,&ticks);
        __atomic_store_n(&hakios_p124_wfi_bounds[i],ticks,__ATOMIC_RELAXED);
    }
    uint64_t span=0,now=mach_absolute_time();
    nanoseconds_to_absolutetime(180ULL*NSEC_PER_SEC,&span);
    __atomic_store_n(&hakios_p125_usb_down_at,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_usb_up_at,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_display_events,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_start,v==3?0:now,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_end,0,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p125_stop_reason,0,__ATOMIC_RELEASE);
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
