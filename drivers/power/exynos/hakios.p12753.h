/* P12.7.5.3: boot-local lower-only thermal guard and explicit delayed
 * battery audit. Existing governor is the only worker; no new power-state
 * operation. Legacy P125/P127 probes retain their original ABI.
 * All new hardware accesses are read-only, to the external PMU or ACPM. */
#ifndef HAKIOS_P12753_H
#define HAKIOS_P12753_H
#include <sys/hakios.exynos_abox.h>
/* XNU 8796's exported machine_routines.h does not declare ml_io_map.
 * Use the same typed declaration as hakios.exynos_mfc.c, which maps
 * this PMU range on S9. No implicit int return or address truncation. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#define P12753_ROWS 96u
#define P53_BIG 1u
#define P53_LIT 2u
#define P53_GPU 4u
#define P53_BAT 8u
#define P53_INVALID 16u
#define P53_DISPLAY 32u
#define P53_LINK 64u
#define P53_DOMAIN 128u
#define P53_BUSY 256u
#define P53_CONFLICT 512u
#define P53_TIMEOUT 1024u
#define P53_CANCEL 2048u
static LCK_GRP_DECLARE(p12753_group,"p12753");
static LCK_SPIN_DECLARE(p12753_lock,&p12753_group);
static unsigned p12753_state,p12753_request,p12753_select,p12753_reason,p12753_mask,p12753_count,p12753_drops,p12753_bus;
static uint32_t p12753_cancel,p12753_bcap,p12753_lcap;
static uint64_t p12753_arm_at,p12753_wait_until,p12753_quiet,p12753_start,p12753_end,p12753_until,p12753_down,p12753_on,p12753_last,p12753_bus_last,p12753_lease,p12753_policy_lease;
static uint64_t p12753_guard_at,p12753_guard_cool,p12753_guard_changes;
static int32_t p12753_peak_big=-273000,p12753_peak_lit=-273000,p12753_prev_big=-273000;
static int32_t p12753_gbig=-273000,p12753_glit=-273000,p12753_ggpu=-273000,p12753_gbat=-273,p12753_slope;
static unsigned p12753_gmask,p12753_gage;
static vm_offset_t p12753_pmu;
static bool p12753_mapped;
static int p12753_revision=1;
SYSCTL_INT(_kern_hakios,OID_AUTO,p12753_revision,CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,&p12753_revision,0,"P12753 ABI");
struct p12753_row {
    uint64_t at,latency;
    uint32_t kind,index,raw,valid,age,bs,ls,bkhz,lkhz,bmeas,lmeas,policy,driver,refresh,g3d,dpu,isppre,mfc,aud,mask,opp_uv,opp_valid;
    int32_t error,big,lit,gpu,bat;
};
static struct p12753_row p12753_rows[P12753_ROWS],p12753_stop_row;
static bool p12753_enabled(void){
    static int cached=-1;int v=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(v<0){int flag=0;v=p12752_boot_enabled() && PE_parse_boot_argn("p12753",&flag,sizeof(flag)) && flag==1;__atomic_store_n(&cached,v,__ATOMIC_RELEASE);}
    return v!=0;
}
static unsigned p12753_temp_mask(int32_t b,int32_t l,int32_t g,int32_t bat,unsigned age,
                                  int32_t bt,int32_t lt,int32_t gt,int32_t batt){
    unsigned m=0;
    if(b<0 || l<0 || g<0 || bat<=0 || age>2000)m|=P53_INVALID;
    if(b>=bt)m|=P53_BIG;
    if(l>=lt)m|=P53_LIT;
    if(g>=gt)m|=P53_GPU;
    if(bat>=batt)m|=P53_BAT;
    return m;
}
/* This cap is independent of experiment ownership. Never modifies OCP,
 * recommended masks, floor, voltage or physical CPU states. */
static void p12753_guard_tick(uint64_t now){
    if(!p12753_enabled() || !gov_running)return;
    if(p12753_guard_at && now-p12753_guard_at<p1274_ticks_ms(200))return;
    int32_t b=hakios_exynos_tmu_zone_now_mc(0),l=hakios_exynos_tmu_zone_now_mc(1);
    int32_t g=hakios_exynos_tmu_zone_mc(2),bat=hakios_battery_temperature();
    unsigned age=(unsigned)MIN(hakios_exynos_tmu_age_ms(),(uint64_t)UINT32_MAX);
    bool off=hakios_display_requested_on()==0;
    uint32_t oldb=__atomic_load_n(&p12753_bcap,__ATOMIC_ACQUIRE),oldl=__atomic_load_n(&p12753_lcap,__ATOMIC_ACQUIRE);
    uint32_t bc=0,lc=0;
    unsigned m=p12753_temp_mask(b,l,g,bat,age,70000,68000,70000,45);
    int32_t slope=p12753_guard_at && now-p12753_guard_at<=p1274_ticks_ms(1000) && b>=0 && p12753_prev_big>=0?b-p12753_prev_big:0;
    bool burst=(off && (clusters[0].sum_now+clusters[1].sum_now>=150u || b>=65000 || l>=64000 || slope>=2000)) || (b>=65000 && slope>=2000);
    if(burst)m|=P53_BUSY;
    if(off)m|=P53_DISPLAY;
    if((m&(P53_BIG|P53_LIT|P53_GPU|P53_BAT|P53_INVALID)) || burst){
        bc=1066000;lc=1053000;p12753_guard_cool=0;
    }else if(oldb==1066000){
        if(b<=63000 && l<=62000 && g<=65000 && bat<44){
            if(!p12753_guard_cool)p12753_guard_cool=now;
        }else p12753_guard_cool=0;
        if(!p12753_guard_cool || now-p12753_guard_cool<p1274_ticks_ms(5000)){bc=1066000;lc=1053000;}
    }
    uint64_t wake=__atomic_load_n(&hakios_p12753_display_on,__ATOMIC_ACQUIRE);
    if(!bc && (off || (wake && now>=wake && now-wake<p1274_ticks_ms(5000)))){bc=1261000;lc=1248000;}
    /* Do not impose a non-existent OPP or undercut the known stock
     * LITTLE floor. Existing storage floor and stock ceiling win. */
    if(bc && !p1273_cap_rate_present(&clusters[1],bc))bc=0;
    if(lc && (lc<clusters[0].floor || lc<clusters[0].dt_min || !p1273_cap_rate_present(&clusters[0],lc)))lc=0;
    lck_spin_lock(&p12753_lock);
    p12753_guard_at=now;p12753_prev_big=b;p12753_slope=slope;
    p12753_gbig=b;p12753_glit=l;p12753_ggpu=g;p12753_gbat=bat;p12753_gage=age;p12753_gmask=m;
    if(b>p12753_peak_big)p12753_peak_big=b;
    if(l>p12753_peak_lit)p12753_peak_lit=l;
    if(bc!=oldb || lc!=oldl)p12753_guard_changes++;
    __atomic_store_n(&p12753_bcap,bc,__ATOMIC_RELEASE);
    __atomic_store_n(&p12753_lcap,lc,__ATOMIC_RELEASE);
    lck_spin_unlock(&p12753_lock);
    if(bc!=oldb || lc!=oldl)say("p12753-guard: at=%llu mask=%u big=%d lit=%d gpu=%d bat=%d slope=%d bcap=%u lcap=%u\n",(unsigned long long)now,m,b,l,g,bat,slope,bc,lc);
}
static uint32_t p12753_ceiling(struct cluster *c,uint32_t stock){
    uint32_t cap=c==&clusters[1]?__atomic_load_n(&p12753_bcap,__ATOMIC_ACQUIRE):__atomic_load_n(&p12753_lcap,__ATOMIC_ACQUIRE);
    return cap?MIN(stock,cap):stock;
}
static int p12753_guard_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[360];bool enabled=p12753_enabled();lck_spin_lock(&p12753_lock);
    snprintf(buf,sizeof(buf),"version=1 enabled=%u at=%llu mask=%u big=%d lit=%d gpu=%d bat=%d age=%u slope=%d bcap=%u lcap=%u peak_big=%d peak_lit=%d changes=%llu",
        enabled?1u:0u,(unsigned long long)p12753_guard_at,p12753_gmask,p12753_gbig,p12753_glit,p12753_ggpu,p12753_gbat,p12753_gage,p12753_slope,p12753_bcap,p12753_lcap,p12753_peak_big,p12753_peak_lit,(unsigned long long)p12753_guard_changes);
    lck_spin_unlock(&p12753_lock);return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12753_guard,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12753_guard_sysctl,"A","P12753 independent boot guard");
static unsigned p12753_policy(unsigned v){
    /* 1=stock, 2=BASE15, 3=PSR, 4=LITTLE949, 5=ABOX, 6=BT,
     * 7=WiFi, 8=TICKER, 9=stock+ACPM audit, 10=stock60. No combined or deep floor. */
    static const unsigned masks[]={0,0,1,9,17,5,257,129,513,0,0};
    return v<sizeof(masks)/sizeof(masks[0])?masks[v]:0;
}
static unsigned p12753_seconds(unsigned v){return (v==1 || v==9)?120u:60u;}
static bool p12753_gate_clear(void){
    bool ok;lck_spin_lock(&p1273_gate_lock);p1273_gate_refresh_locked(mach_absolute_time());ok=p1273_gate_phase==0;lck_spin_unlock(&p1273_gate_lock);return ok;
}
static bool p12753_qualified(unsigned request){
    unsigned policy=p12753_policy(request),bit=p1273_gate_bit(policy);
    bool ok=true;
    if(bit){lck_spin_lock(&p1273_gate_lock);p1273_gate_refresh_locked(mach_absolute_time());
        ok=p1273_gate_phase==0 && (p1273_gate_qualified&bit)==bit;lck_spin_unlock(&p1273_gate_lock);}
    if(request==3){
        /* Existing PSR path is not in FunctionalGate. Require at least
         * one successful mask/unmask pair this boot and no known fault.
         * Host additionally requires explicit connected A/B/A + wake review. */
        uint64_t m=__atomic_load_n(&hakios_p127_psr[0],__ATOMIC_ACQUIRE);
        uint64_t u=__atomic_load_n(&hakios_p127_psr[1],__ATOMIC_ACQUIRE);
        ok=m && m==u && !__atomic_load_n(&hakios_p127_psr[3],__ATOMIC_ACQUIRE);
    }
    return ok;
}
static bool p12753_other_idle(void){
    lck_spin_lock(&p12752_lock);bool old_idle=p12752_idle_mode==0;lck_spin_unlock(&p12752_lock);
    return gov_running && !ocp_on && gov_policy==2 && clusters[0].online==4 && clusters[1].online==4 &&
        big_cores==SCION_BIG_CORES && !hakios_cpu_exit_mask && hakios_p124_ufs_safe() &&
        p1273_gate_idle() && p12753_gate_clear() && old_idle && !__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE);
}
static uint32_t p12753_pmu_read(unsigned offset){
    if(!p12753_pmu || offset>0x4ffcu || (offset&3u))return UINT32_MAX;
    return *(volatile uint32_t *)(p12753_pmu+offset);
}
static void p12753_append(const struct p12753_row *r){
    lck_spin_lock(&p12753_lock);
    if(p12753_count<P12753_ROWS)p12753_rows[p12753_count++]=*r;
    else p12753_drops++;
    lck_spin_unlock(&p12753_lock);
}
static void p12753_sample(struct p12753_row *r,uint64_t now){
    r->at=now;r->kind=1;r->big=hakios_exynos_tmu_zone_now_mc(0);r->lit=hakios_exynos_tmu_zone_now_mc(1);
    r->gpu=hakios_exynos_tmu_zone_mc(2);r->bat=hakios_battery_temperature();
    r->age=(unsigned)MIN(hakios_exynos_tmu_age_ms(),(uint64_t)UINT32_MAX);
    r->bs=clusters[1].sum_now;r->ls=clusters[0].sum_now;r->bkhz=clusters[1].cur;r->lkhz=clusters[0].cur;
    r->bmeas=clusters[1].measured;r->lmeas=clusters[0].measured;r->policy=hakios_p127_mode();r->driver=hakios_p126_active(P126_MASK);
    r->g3d=p12753_pmu_read(0x4044u);
    r->dpu=p12753_pmu_read(EXYNOS_CAM_PMU_DPU_STATUS);
    r->isppre=p12753_pmu_read(EXYNOS_CAM_PMU_ISPPRE_STATUS);
    r->mfc=p12753_pmu_read(EXYNOS_MFC_PMU_STATUS);
    r->aud=p12753_pmu_read(PMU_AUD_STATUS);
    /* Never touch DECON if the external PMU status is not confirmed ON. */
    r->refresh=r->dpu!=UINT32_MAX && (r->dpu&15u)==15u?hakios_scanout_refreshes():UINT32_MAX;
    r->mask=p12753_temp_mask(r->big,r->lit,r->gpu,r->bat,r->age,67000,66000,68000,45);
    r->valid=(r->mask&P53_INVALID)==0;
}
static void p12753_finish(unsigned reason,unsigned mask,uint64_t now,const struct p12753_row *r){
    uint64_t end=reason==10?p12753_until:now;
    uint64_t lease=p12753_lease;
    /* Only release the lease we actually acquired. No reset of counters. */
    if(lease && __atomic_compare_exchange_n(&hakios_p125_lease,&lease,0,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        __atomic_store_n(&hakios_p125_end,end,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_stop_reason,reason,__ATOMIC_RELEASE);
    }else if(p12753_lease && reason==10){reason=6;mask|=P53_CONFLICT;}
    __atomic_store_n(&hakios_p12753_observe,0,__ATOMIC_RELEASE);
    if(p12753_policy_lease && __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)==p12753_policy_lease)
        hakios_p127_stop(reason);
    lck_spin_lock(&p12753_lock);
    p12753_end=end;p12753_reason=reason;p12753_mask=mask;p12753_stop_row=*r;
    p12753_state=3;
    __atomic_store_n(&hakios_p12753_busy,0,__ATOMIC_RELEASE);
    lck_spin_unlock(&p12753_lock);
    say("p12753-audit: frozen request=%u reason=%u mask=%u start=%llu end=%llu\n",p12753_request,reason,mask,(unsigned long long)p12753_start,(unsigned long long)end);
}
static int p12753_arm_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE)?(int)p12753_request:0;
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    if(v<0 || v>10)return EINVAL;
    if(!v){if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))__atomic_store_n(&p12753_cancel,1,__ATOMIC_RELEASE);return 0;}
    if(!p12753_enabled() || !p12753_other_idle() || !p12753_qualified((unsigned)v) || p125_thermal_reason() || hakios_battery_temperature()<=0)return EPERM;
    uint64_t now=mach_absolute_time();
    uint64_t fg=__atomic_load_n(&hakios_p12753_fg_link,__ATOMIC_ACQUIRE);
    if((fg&3u)!=3u || now<(fg&~3ULL) || now-(fg&~3ULL)>p1274_ticks_ms(15000))return EPERM;
    lck_spin_lock(&p12753_lock);
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE)){lck_spin_unlock(&p12753_lock);return EBUSY;}
    p12753_request=(unsigned)v;p12753_arm_at=now;p12753_wait_until=now+p1274_ticks_ms(90000);
    p12753_quiet=p12753_start=p12753_end=p12753_until=p12753_down=p12753_on=p12753_last=p12753_bus_last=p12753_lease=p12753_policy_lease=0;
    p12753_reason=p12753_mask=p12753_count=p12753_drops=p12753_bus=p12753_select=0;
    bzero(&p12753_stop_row,sizeof(p12753_stop_row));__atomic_store_n(&p12753_cancel,0,__ATOMIC_RELEASE);
    p12753_state=1;__atomic_store_n(&hakios_p12753_busy,1,__ATOMIC_RELEASE);
    lck_spin_unlock(&p12753_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12753_arm,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12753_arm_sysctl,"I","P12753 arm one offline audit 1..10, 0 cancel");
static void p12753_audit_tick(uint64_t now){
    if(!__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return;
    /* One PMU mapping on the existing worker, not in an IRQ or MMIO
     * inside a powered-off domain. The exact offsets are existing S9 ones. */
    if(!p12753_mapped){p12753_pmu=ml_io_map(EXYNOS_PMU_PA,0x5000);p12753_mapped=true;}
    if(p12753_last && now-p12753_last<p1274_ticks_ms(200))return;
    p12753_last=now;
    struct p12753_row r={0};p12753_sample(&r,now);
    unsigned hard=p12753_temp_mask(r.big,r.lit,r.gpu,r.bat,r.age,76000,70000,72000,47);
    uint64_t fg=__atomic_load_n(&hakios_p12753_fg_link,__ATOMIC_ACQUIRE),at=fg&~3ULL;
    uint64_t down=__atomic_load_n(&hakios_p12753_usb_down,__ATOMIC_ACQUIRE);
    uint64_t up=__atomic_load_n(&hakios_p12753_usb_up,__ATOMIC_ACQUIRE);
    uint64_t on=__atomic_load_n(&hakios_p12753_display_on,__ATOMIC_ACQUIRE);
    bool fresh=(fg&2u) && now>=at && now-at<=p1274_ticks_ms(15000);
    bool off=hakios_display_requested_on()==0;
    bool gpuoff=r.g3d!=UINT32_MAX && (r.g3d&15u)==0;
    if(__atomic_load_n(&p12753_cancel,__ATOMIC_ACQUIRE)){p12753_finish(6,P53_CANCEL,now,&r);return;}
    if(hard&(P53_BIG|P53_LIT|P53_GPU|P53_BAT)){p12753_finish(2,hard,now,&r);return;}
    if(p12753_state==1){
        if(down>p12753_arm_at){lck_spin_lock(&p12753_lock);p12753_down=down;lck_spin_unlock(&p12753_lock);}
        if(now>=p12753_wait_until){p12753_finish(8,P53_TIMEOUT,now,&r);return;}
        if(!p12753_other_idle()){p12753_finish(6,P53_CONFLICT,now,&r);return;}
        if(down>p12753_arm_at && up>down){p12753_finish(8,P53_LINK,now,&r);return;}
        bool quiet=down>p12753_arm_at && down>up && fresh && !(fg&1u) && at>=down && off && gpuoff &&
            !r.mask && r.bs+r.ls<150u && !hakios_p124_ufs_busy() && hakios_p123_input_quiet(now);
        if(!quiet || (p12753_quiet && on>=p12753_quiet)){p12753_quiet=0;return;}
        if(!p12753_quiet){p12753_quiet=now;return;}
        if(now-p12753_quiet<p1274_ticks_ms(20000))return;
        unsigned policy=p12753_policy(p12753_request);
        /* All hardware module admission checks remain in the original
         * P127 arm helper, including UFS/OPP and functional gate ownership. */
        if(!p12753_qualified(p12753_request) || (policy && p127_arm_mode((int)policy))){p12753_finish(6,P53_CONFLICT,now,&r);return;}
        uint64_t start=mach_absolute_time();
        uint64_t until=(start+p1274_ticks_ms((uint64_t)p12753_seconds(p12753_request)*1000))&~3ULL;
        if(policy){
            uint64_t orig=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
            uint64_t clipped=((until+p1274_ticks_ms(1000))&~(uint64_t)P127_MASK)|policy;
            /* Only shorten; do not extend the pre-existing lease. */
            if((orig&P127_MASK)!=policy){p12753_finish(6,P53_CONFLICT,now,&r);return;}
            p12753_policy_lease=orig;
            if((orig&~(uint64_t)P127_MASK)<(clipped&~(uint64_t)P127_MASK) ||
               !__atomic_compare_exchange_n(&hakios_p127_lease,&orig,clipped,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
                p12753_finish(6,P53_CONFLICT,now,&r);return;
            }
            p12753_policy_lease=clipped;
        }
        if(hakios_display_requested_on()!=0 || __atomic_load_n(&hakios_p12753_display_on,__ATOMIC_ACQUIRE)!=on ||
           __atomic_load_n(&hakios_p12753_usb_down,__ATOMIC_ACQUIRE)!=down || __atomic_load_n(&hakios_p12753_usb_up,__ATOMIC_ACQUIRE)>down){
            p12753_finish(4,P53_DISPLAY|P53_LINK,start,&r);return;
        }
        const unsigned us[]={100,500,1000,2000,3500,10000,20000,50000};
        for(unsigned i=0;i<8;i++){uint64_t ticks=0;nanoseconds_to_absolutetime((uint64_t)us[i]*1000,&ticks);__atomic_store_n(&hakios_p127_bounds[i],ticks,__ATOMIC_RELAXED);}
        const unsigned oldus[]={100,500,1000,10000};
        for(unsigned i=0;i<4;i++){uint64_t ticks=0;nanoseconds_to_absolutetime((uint64_t)oldus[i]*1000,&ticks);__atomic_store_n(&hakios_p124_wfi_bounds[i],ticks,__ATOMIC_RELAXED);}
        __atomic_store_n(&hakios_p125_start,start,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_end,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_stop_reason,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_usb_down_at,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_usb_up_at,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p125_display_events,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p1273_offline_fixed,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p1273_offline_deadline,0,__ATOMIC_RELEASE);
        __atomic_store_n(&hakios_p12753_observe,1,__ATOMIC_RELEASE);
        uint64_t expected=0,lease=until|1u;
        if(!__atomic_compare_exchange_n(&hakios_p125_lease,&expected,lease,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
            p12753_finish(6,P53_CONFLICT,start,&r);return;
        }
        lck_spin_lock(&p12753_lock);
        p12753_lease=lease;p12753_start=start;p12753_until=until;p12753_down=down;p12753_on=on;p12753_state=2;
        lck_spin_unlock(&p12753_lock);
        say("p12753-audit: measure request=%u down=%llu start=%llu until=%llu\n",p12753_request,(unsigned long long)down,(unsigned long long)start,(unsigned long long)until);
        return;
    }
    if(p12753_state!=2)return;
    unsigned mask=r.mask;
    /* Qualification invalidated by a driver fault or time expiry must
     * terminate the active phase, not just mark it after the fact. */
    if(p12753_request!=3 && !p12753_qualified(p12753_request))mask|=P53_CONFLICT;
    if(p12753_request==3 && __atomic_load_n(&hakios_p127_psr[3],__ATOMIC_ACQUIRE))mask|=P53_CONFLICT;
    if(!off || on!=p12753_on || __atomic_load_n(&hakios_p125_display_events,__ATOMIC_ACQUIRE))mask|=P53_DISPLAY;
    if(!fresh || (fg&1u) || down!=p12753_down || up>p12753_down)mask|=P53_LINK;
    if(!gpuoff)mask|=P53_DOMAIN;
    if(r.bs+r.ls>=200u)mask|=P53_BUSY;
    if(!gov_running || ocp_on || gov_policy!=2 || clusters[0].online!=4 || clusters[1].online!=4 || big_cores!=SCION_BIG_CORES ||
       hakios_cpu_exit_mask || !hakios_p124_ufs_safe() || hakios_p124_ufs_busy() ||
       __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE)!=p12753_lease ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)!=p12753_policy_lease ||
       __atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) || __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE))mask|=P53_CONFLICT;
    if(mask){
        r.mask=mask;p12753_append(&r);
        unsigned reason=(mask&(P53_BIG|P53_LIT|P53_GPU|P53_BAT))?2:(mask&P53_INVALID)?3:(mask&P53_DISPLAY)?4:8;
        p12753_finish(reason,mask,now,&r);return;
    }
    if(now>=p12753_until){p12753_finish(10,0,now,&r);return;}
    if(!p12753_bus_last || now-p12753_bus_last>=p1274_ticks_ms(4000)){
        /* Bus-only diagnostic window, never contaminate ordinary power
         * phases with repeated ACPM queries. */
        if(p12753_request==9 && p12753_bus<13){
            struct p12753_row br={0};br.kind=2;
            lck_spin_lock(&p12753_lock);br.index=p12753_bus++;lck_spin_unlock(&p12753_lock);
            uint64_t t=mach_absolute_time();br.error=hakios_acpm_dvfs_get(br.index,&br.raw);
            br.at=mach_absolute_time();br.latency=br.at-t;br.valid=!br.error && br.raw!=0;
            if(br.valid){
                uint32_t khz[LEVELS_MOST],uv[LEVELS_MOST];
                unsigned n=hakios_acpm_dvfs_table(br.index,khz,uv,LEVELS_MOST,say);
                for(unsigned i=0;i<n && i<LEVELS_MOST;i++)if(khz[i]==br.raw){br.opp_uv=uv[i];br.opp_valid=uv[i]!=0;break;}
            }
            p12753_append(&br);p12753_bus_last=now;
        }
    }
    /* r.latency is unused for thermal rows: use a separate row cadence. */
    static uint64_t previous_row,previous_phase;
    if(previous_phase!=p12753_start || now-previous_row>=p1274_ticks_ms(2000)){
        p12753_append(&r);previous_row=now;previous_phase=p12753_start;
    }
}
static int p12753_status_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[500];uint64_t hz=0;nanoseconds_to_absolutetime(NSEC_PER_SEC,&hz);
    lck_spin_lock(&p12753_lock);
    snprintf(buf,sizeof(buf),"version=1 state=%u request=%u arm=%llu down=%llu start=%llu end=%llu until=%llu up=%llu now=%llu hz=%llu reason=%u mask=%u count=%u drops=%u bus=%u stop_big=%d stop_lit=%d stop_gpu=%d stop_bat=%d stop_age=%u policy=%u",
        p12753_state,p12753_request,(unsigned long long)p12753_arm_at,(unsigned long long)p12753_down,(unsigned long long)p12753_start,(unsigned long long)p12753_end,(unsigned long long)p12753_until,
        (unsigned long long)__atomic_load_n(&hakios_p12753_usb_up,__ATOMIC_ACQUIRE),(unsigned long long)mach_absolute_time(),(unsigned long long)hz,p12753_reason,p12753_mask,p12753_count,p12753_drops,p12753_bus,p12753_stop_row.big,p12753_stop_row.lit,p12753_stop_row.gpu,p12753_stop_row.bat,p12753_stop_row.age,p12753_policy(p12753_request));
    lck_spin_unlock(&p12753_lock);return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12753_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12753_status_sysctl,"A","P12753 delayed offline state and first-stop sensors");
static int p12753_select_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p12753_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    lck_spin_lock(&p12753_lock);
    if(p12753_state!=3 || v<0 || (unsigned)v>=p12753_count){lck_spin_unlock(&p12753_lock);return EBUSY;}
    __atomic_store_n(&p12753_select,(unsigned)v,__ATOMIC_RELEASE);
    lck_spin_unlock(&p12753_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12753_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12753_select_sysctl,"I","P12753 frozen row selector");
static int p12753_page_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[512];struct p12753_row r;uint64_t phase;
    lck_spin_lock(&p12753_lock);unsigned s=p12753_select;
    if(p12753_state!=3 || s>=p12753_count){lck_spin_unlock(&p12753_lock);return EBUSY;}
    r=p12753_rows[s];phase=p12753_start;lck_spin_unlock(&p12753_lock);
    snprintf(buf,sizeof(buf),"version=1 select=%u phase=%llu at=%llu kind=%u index=%u raw=%u valid=%u errno=%d latency=%llu big=%d lit=%d gpu=%d bat=%d age=%u bs=%u ls=%u bkhz=%u lkhz=%u bmeas=%u lmeas=%u policy=%u driver=%u refresh=%u g3d=%u dpu=%u isppre=%u mfc=%u aud=%u mask=%u opp_uv=%u opp_valid=%u",
        s,(unsigned long long)phase,(unsigned long long)r.at,r.kind,r.index,r.raw,r.valid,r.error,(unsigned long long)r.latency,r.big,r.lit,r.gpu,r.bat,r.age,r.bs,r.ls,r.bkhz,r.lkhz,r.bmeas,r.lmeas,r.policy,r.driver,r.refresh,r.g3d,r.dpu,r.isppre,r.mfc,r.aud,r.mask,r.opp_uv,r.opp_valid);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12753_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12753_page_sysctl,"A","P12753 frozen audit row");
#endif
