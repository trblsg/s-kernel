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
static bool
p127_floor_safe(unsigned mode)
{
    uint32_t target=p127_floor_khz(mode);
    if(!target)return false;
    if(!p124_enabled || !gov_running || gov_policy!=2 ||
       __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
       clusters[0].online!=4 || clusters[1].online!=4 ||
       hakios_cpu_exit_mask || big_cores!=SCION_BIG_CORES ||
       clusters[0].floor!=1053000 || target<clusters[0].dt_min ||
       !hakios_p124_ufs_safe() || hakios_p124_ufs_busy() ||
       !hakios_p123_input_quiet(mach_absolute_time()) ||
       clusters[0].load>=40 || clusters[0].thermal!=0)return false;
    for(unsigned i=0;i<clusters[0].n;i++)if(clusters[0].levels[i]==target)return true;
    return false;
}
static void
p127_guard(void)
{
    uint64_t lease=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
    if(!lease)return;
    unsigned reason=p125_thermal_reason();
    if(!reason && !hakios_p127_mode())reason=1;
    unsigned mode=hakios_p127_mode();
    /* Busy I/O/load bypasses the floor at each governor decision; a
     * topology or ownership change revokes the whole experiment. */
    if(!reason && P127_FLOOR(mode) && (!gov_running || gov_policy!=2 ||
       clusters[0].online!=4 || clusters[1].online!=4 ||
       hakios_cpu_exit_mask || !hakios_p124_ufs_safe()))reason=6;
    if(reason)hakios_p127_stop(reason);
}
static int
p127_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *arg1,
    __unused int arg2,struct sysctl_req *req)
{
    p127_guard();
    int v=(int)hakios_p127_mode();
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || req->newptr==USER_ADDR_NULL)return e;
    if(v<0 || v>(int)P127_MASK || P127_FLOOR(v)>5)return EINVAL;
    if(!v){hakios_p127_stop(0);return 0;}
    if(!(v&P127_BASE))return EINVAL;
    /* First qualification: a single extra policy per baseline lease. */
    unsigned extras=(unsigned)v&~(P127_BASE|0x70u);
    if((extras && (extras&(extras-1))) || (extras && P127_FLOOR(v)))return EINVAL;
    if(__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE))return EBUSY;
    if(p125_thermal_reason() || (P127_FLOOR(v) && !p127_floor_safe((unsigned)v)))return EPERM;
    uint64_t span=0,expected=0;
    nanoseconds_to_absolutetime(((v&(P127_ABOX|P127_PSR|P127_WIFI|P127_BT|P127_TICKER)) || P127_FLOOR(v)?120ULL:420ULL)*NSEC_PER_SEC,&span);
    const unsigned us[]={100,500,1000,2000,3500,10000};
    for(unsigned i=0;i<6;i++){
        uint64_t ticks=0;
        nanoseconds_to_absolutetime((uint64_t)us[i]*1000,&ticks);
        __atomic_store_n(&hakios_p127_bounds[i],ticks,__ATOMIC_RELAXED);
    }
    uint64_t next=((mach_absolute_time()+span)&~(uint64_t)P127_MASK)|(unsigned)v;
    if(!__atomic_compare_exchange_n(&hakios_p127_lease,&expected,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    __atomic_store_n(&hakios_p127_reason,0,__ATOMIC_RELEASE);
    if(hakios_display_requested_on()!=0){hakios_p127_stop(4);return EPERM;}
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,
    p127_mode_sysctl,"I","P12.7 base1 USB2 ABOX4 PSR8 LITTLE selector bits4..6, WiFi INTx128 BT256 ticker512, bounded OFF lease");
/* Read-only ACPM requests are made only on the existing governor worker,
 * never in USB sysctl. Keep rates/errors cached, no bus DVFS writes. */
static uint32_t p127_bus_khz[6],p127_bus_valid;
static uint64_t p127_bus_at;
static void
p127_bus_poll(uint64_t now)
{
    if(!hakios_p127_mode() || !hakios_p125_active())return;
    uint64_t interval=0;nanoseconds_to_absolutetime(10ULL*NSEC_PER_SEC,&interval);
    if(p127_bus_at && now-p127_bus_at<interval)return;
    const unsigned idx[]={0,1,5,7,8,9};unsigned valid=0;
    for(unsigned i=0;i<6;i++){
        uint32_t khz=0;
        if(!hakios_acpm_dvfs_get(idx[i],&khz) && khz)valid|=1u<<i;
        __atomic_store_n(&p127_bus_khz[i],khz,__ATOMIC_RELAXED);
    }
    __atomic_store_n(&p127_bus_valid,valid,__ATOMIC_RELEASE);
    __atomic_store_n(&p127_bus_at,now,__ATOMIC_RELEASE);
}
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
    if(cpu<0 || cpu>=8)return EINVAL;
    if(__atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    char buf[448];size_t at=0;
    int n=snprintf(buf,sizeof(buf),"version=1 cpu=%d sgi=%llx ppi=%llx spi=%llx bins=",cpu,
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][0],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][1],__ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&hakios_p127_irq[cpu][2],__ATOMIC_RELAXED));
    if(n<0 || (size_t)n>=sizeof(buf))return EOVERFLOW;
    at=(size_t)n;
    for(unsigned i=0;i<7;i++){
        n=snprintf(buf+at,sizeof(buf)-at,"%s%llx/%llx",i?",":"",
            (unsigned long long)__atomic_load_n(&hakios_p127_wfi_n[cpu][i],__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&hakios_p127_wfi_ticks[cpu][i],__ATOMIC_RELAXED));
        if(n<0 || (size_t)n>=sizeof(buf)-at)return EOVERFLOW;
        at+=(size_t)n;
    }
    return SYSCTL_OUT(req,buf,at+1);
}
#define P127_WFI(N) SYSCTL_PROC(_kern_hakios,OID_AUTO,p127_wfi##N,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,N,p127_wfi_sysctl,"A","P12.7 WFI count/ticks bins")
P127_WFI(0); P127_WFI(1); P127_WFI(2); P127_WFI(3); P127_WFI(4); P127_WFI(5); P127_WFI(6); P127_WFI(7);
#undef P127_WFI
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
    unsigned p127=hakios_p127_mode();
    uint64_t lease=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
    nanoseconds_to_absolutetime((p127 && (v==1 || v==3)?360ULL:180ULL)*NSEC_PER_SEC,&span);
    if(p127 && (lease&~(uint64_t)P127_MASK)<=now+span){
        uint64_t end=lease&~(uint64_t)P127_MASK;
        if(end<=now)return EPERM;
        span=end-now;
    }
    __atomic_store_n(&hakios_p127_offline_aggregate,(p127 && v==3)?1u:0u,__ATOMIC_RELEASE);
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
