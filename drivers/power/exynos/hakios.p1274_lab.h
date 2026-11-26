/* P12.7.4 one-shot late OFF lab. Included after the stock governor helpers.
 * All writes are opt-in, lower-only, no hardware CPU power-state changes.
 * Gov thread owns sampling, policy transitions and frozen timeline. */
#ifndef HAKIOS_P1274_LAB_H
#define HAKIOS_P1274_LAB_H
#include "hakios.p1274_policy.h"
#define P1274_ROWS 480u
#define P1274_EVENTS 2048u
#define P1274_DETAIL 128u
#define P1274_RATE_EVENTS 512u
extern uint32_t hakios_p1274_recommended_mask(void);
static uint32_t p1274_state,p1274_profile_id,p1274_cancel,p1274_reason,p1274_select,p1274_pending_finish;
static uint32_t p1274_fast_limit,p1274_trigger_bits,p1274_count,p1274_event_count,p1274_event_drops;
static uint32_t p1274_big_mask,p1274_lit_mask,p1274_requested_mask;
static uint32_t p1274_rate_count,p1274_rate_drops;
struct p1274_rate_event {uint64_t at,began,tmu_at;uint32_t type,from,to,cap,asked;int32_t error,mc;};
static struct p1274_rate_event p1274_rate_events[P1274_RATE_EVENTS];
static uint64_t p1274_start,p1274_end,p1274_planned_end,p1274_high_since,p1274_last_row,p1274_last_tick;
static uint64_t p1274_tmu_at,p1274_anchor_at,p1274_cool_since,p1274_measured_at;
static int32_t p1274_mc=-273000,p1274_anchor_mc=-273000,p1274_slope_mc;
static bool p1274_rising;
static uint64_t p1274_burst_start,p1274_burst_end,p1274_idle_since;
static uint64_t p1274_measure_ok,p1274_measure_bad,p1274_measure_skip;
static uint64_t p1274_weight,p1274_big_weight,p1274_lit_weight,p1274_big_sum_weight,p1274_lit_sum_weight;
static uint32_t p1274_big_peak,p1274_lit_peak,p1274_big_sum_peak,p1274_lit_sum_peak;
static int32_t p1274_temp_peak=-273000;
struct p1274_row {
    uint64_t at,tmu_at,measured_at;
    int32_t direct_mc,peak_mc,lit_mc,gpu_mc,slope_mc;
    uint32_t bp,lp,ba,la,bs,ls,bsp,lsp,bkhz,lkhz,bcap,fast,measured;
    uint32_t online,recommended,requested,display,profile;
};
static struct p1274_row p1274_rows[P1274_ROWS];
/* User supplied scalar evidence. Producer PID/task/TID stamped by kernel;
 * payload (including subject PID for cputop) remains advisory. No strings,
 * addresses, pointers or policy commands accepted. One bounded array. */
struct p1274_event {uint64_t at,task,tid,v[8];uint32_t pid;};
static struct p1274_event p1274_events[P1274_EVENTS];
struct p1274_producer {uint64_t task,at;uint32_t n;};
static struct p1274_producer p1274_producers[128];
static LCK_GRP_DECLARE(p1274_event_group,"p1274-events");
static LCK_SPIN_DECLARE(p1274_event_lock,&p1274_event_group);
struct p1274_detail {
    uint64_t bin,at,weight,bw,lw,bsw,lsw,tmu_at;
    uint32_t bp,lp,bsp,lsp,bkhz,lkhz;
    int32_t mc,peak_mc;
    uint32_t valid;
};
static struct p1274_detail p1274_detail[P1274_DETAIL];

static uint64_t p1274_ticks_ms(uint64_t ms) {
    uint64_t t=0;nanoseconds_to_absolutetime(ms*NSEC_PER_MSEC,&t);return t;
}
static bool p1274_boot_enabled(void) {
    int v=0,p=0;
    return PE_parse_boot_argn("p1274",&v,sizeof(v)) && v==1 &&
           PE_parse_boot_argn("p1273",&p,sizeof(p)) && p==1;
}
static bool p1274_conflict(void) {
    unsigned cap=__atomic_load_n(&p1273_cap_state,__ATOMIC_ACQUIRE);
    return !gov_running || gov_policy!=2 || clusters[0].online!=4 || clusters[1].online!=4 ||
        hakios_cpu_exit_mask || __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE) || cap==2 || cap==3;
}
static unsigned p1274_active_profile(uint64_t now) {
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=2 ||
        __atomic_load_n(&p1274_cancel,__ATOMIC_ACQUIRE) ||
        hakios_display_requested_on()!=0 || __atomic_load_n(&hakios_p1274_display_on_at,__ATOMIC_ACQUIRE))return 0;
    uint64_t end=__atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE);
    if(!end || now>=end)return 0;
    return __atomic_load_n(&p1274_profile_id,__ATOMIC_ACQUIRE);
}
static uint32_t p1274_ceiling(struct cluster *c,uint32_t stock) {
    if(c!=&clusters[1])return stock;
    uint32_t fast=__atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE);
    if(fast)stock=MIN(stock,fast);
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=2)return stock;
    uint64_t now=mach_absolute_time();
    unsigned p=p1274_active_profile(now);
    if(p && (p1274_conflict() || (p>=P1274_FAST && !hakios_p123_input_quiet(now))))p=0;
    uint32_t cap=s9_p1274_cap(p);
    return cap?MIN(stock,cap):stock;
}
static uint32_t p1274_cores(uint32_t stock,uint64_t now) {
    unsigned p=p1274_active_profile(now);
    if(p>=P1274_REC2 && !p1274_conflict() && hakios_p123_input_quiet(now))return s9_p1274_cores(p);
    return stock;
}
static bool p1274_skip_big_measure(void) {
    /* Retain all safety probes. R0 is refused if OCP requires BIG binding. */
    return __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)==2 &&
        p1274_active_profile(mach_absolute_time())==P1274_REC0 && big_cores==0;
}
static void p1274_measure_sample(uint64_t now,bool skipped) {
    unsigned p=p1274_active_profile(now);
    if(!p)return;
    p1274_measured_at=now;
    if(skipped){p1274_measure_skip++;return;}
    uint32_t cap=s9_p1274_cap(p);
    if(cap){
        if(clusters[1].cur<=cap && p1273_rate_matches(clusters[1].measured,clusters[1].cur))
            p1274_measure_ok++;
        else p1274_measure_bad++;
    }
}
static void p1274_topology(void) {
    const ml_topology_info_t *t=ml_get_topology_info();
    p1274_big_mask=p1274_lit_mask=0;
    for(unsigned i=0;i<t->num_cpus && i<MAX_CPUS;i++){
        unsigned id=t->cpus[i].cpu_id,aff=(t->cpus[i].phys_id>>8)&255u;
        if(id>=32)continue;
        if(aff==clusters[1].affinity)p1274_big_mask|=1u<<id;
        if(aff==clusters[0].affinity)p1274_lit_mask|=1u<<id;
    }
}
static void p1274_requested(unsigned cores,uint32_t mask) {
    (void)cores;
    if(!p1274_big_mask)return;
    __atomic_store_n(&p1274_requested_mask,mask & (p1274_big_mask|p1274_lit_mask),__ATOMIC_RELEASE);
}
static void p1274_rate_event(unsigned type,uint64_t began,uint32_t from,uint32_t to,uint32_t cap,unsigned asked,int error) {
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=2)return;
    if(p1274_rate_count>=P1274_RATE_EVENTS){p1274_rate_drops++;return;}
    struct p1274_rate_event *r=&p1274_rate_events[p1274_rate_count++];
    r->at=mach_absolute_time();r->began=began;r->tmu_at=p1274_tmu_at;
    r->type=type;r->from=from;r->to=to;r->cap=cap;r->asked=asked;
    r->error=error;r->mc=p1274_mc;
}
static void p1274_dvfs_record(struct cluster *c,uint64_t began,uint32_t target,uint32_t max,int error) {
    if(c!=&clusters[1] || __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=2)return;
    if(p1274_mc>=65000 || __atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE) ||
       (began>=p1274_ticks_ms(240000) && s9_p1274_cap(p1274_profile_id)))
        p1274_rate_event(2,began,target,c->cur,max,c->asked,error);
}
static void p1274_fast_tick(uint64_t now,unsigned profile) {
    uint32_t old=__atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE),before=old;
    if(!profile && !old)return;
    if(p1274_tmu_at && now-p1274_tmu_at<p1274_ticks_ms(200))return;
    p1274_tmu_at=now;
    int32_t mc=hakios_exynos_tmu_zone_now_mc(0);
    p1274_mc=mc;p1274_rising=false;
    if(s9_p1274_temp_valid(mc)){
        if(p1274_anchor_at && now>p1274_anchor_at &&
           now-p1274_anchor_at<=p1274_ticks_ms(2000) &&
           s9_p1274_temp_valid(p1274_anchor_mc)){
            p1274_slope_mc=mc-p1274_anchor_mc;
            p1274_rising=p1274_slope_mc>=3000;
        }else p1274_slope_mc=0;
        if(!p1274_anchor_at || now-p1274_anchor_at>=p1274_ticks_ms(1000)){
            p1274_anchor_at=now;p1274_anchor_mc=mc;
        }
        int32_t lit=hakios_exynos_tmu_zone_mc(1),gpu=hakios_exynos_tmu_zone_mc(2);
        int bat=hakios_battery_temperature();
        if(mc<=68000 && !p1274_rising && hakios_exynos_tmu_age_ms()<=2000 &&
           lit>=0 && lit<=65000 && gpu>=0 && gpu<=68000 && bat>0 && bat<44){
            if(!p1274_cool_since)p1274_cool_since=now;
            if(old && now-p1274_cool_since>=p1274_ticks_ms(5000)){
                old=0; /* stock thermal/OCP remain independent */
                p1274_cool_since=0;
            }
        }else p1274_cool_since=0;
    }else{
        p1274_anchor_at=0;p1274_cool_since=0;p1274_slope_mc=0;
        /* Unknown is not a cool reading. Abort the lab and retain existing
         * fast ceiling; if no ceiling, choose a conservative lower bound. */
        if(!old)old=1469000;
    }
    uint32_t cap=s9_p1274_fast_cap(profile,mc,p1274_rising,old);
    __atomic_store_n(&p1274_fast_limit,cap,__ATOMIC_RELEASE);
    if(cap!=before)p1274_rate_event(1,now,before,cap,cap,0,0);
}
static void p1274_detail_record(uint64_t now,uint64_t dt) {
    uint64_t start=__atomic_load_n(&hakios_p1273_trace_start,__ATOMIC_ACQUIRE);
    uint64_t span=__atomic_load_n(&hakios_p1273_trace_span,__ATOMIC_ACQUIRE);
    if(!span || now<start)return;
    uint64_t bin=(now-start)/span;
    struct p1274_detail *r=&p1274_detail[bin%P1274_DETAIL];
    if(!r->valid || r->bin!=bin){memset(r,0,sizeof(*r));r->bin=bin;r->valid=1;r->peak_mc=-273000;}
    r->at=now;r->weight+=dt;r->bw+=dt*clusters[1].load;r->lw+=dt*clusters[0].load;
    r->bsw+=dt*clusters[1].sum_now;r->lsw+=dt*clusters[0].sum_now;
    r->bp=MAX(r->bp,clusters[1].load);r->lp=MAX(r->lp,clusters[0].load);
    r->bsp=MAX(r->bsp,clusters[1].sum_now);r->lsp=MAX(r->lsp,clusters[0].sum_now);
    r->bkhz=clusters[1].cur;r->lkhz=clusters[0].cur;r->mc=p1274_mc;r->tmu_at=p1274_tmu_at;
    if(s9_p1274_temp_valid(p1274_mc))r->peak_mc=MAX(r->peak_mc,p1274_mc);
}
static void p1274_record(uint64_t now,uint64_t dt,bool final) {
    struct cluster *b=&clusters[1],*l=&clusters[0];
    p1274_weight+=dt;p1274_big_weight+=dt*b->load;p1274_lit_weight+=dt*l->load;
    p1274_big_sum_weight+=dt*b->sum_now;p1274_lit_sum_weight+=dt*l->sum_now;
    p1274_big_peak=MAX(p1274_big_peak,b->load);p1274_lit_peak=MAX(p1274_lit_peak,l->load);
    p1274_big_sum_peak=MAX(p1274_big_sum_peak,b->sum_now);p1274_lit_sum_peak=MAX(p1274_lit_sum_peak,l->sum_now);
    if(s9_p1274_temp_valid(p1274_mc))p1274_temp_peak=MAX(p1274_temp_peak,p1274_mc);
    if(!final && p1274_last_row && now-p1274_last_row<p1274_ticks_ms(1000))return;
    uint32_t n=__atomic_load_n(&p1274_count,__ATOMIC_RELAXED);
    if(n>=P1274_ROWS)return;
    struct p1274_row *r=&p1274_rows[n];
    r->at=now;r->tmu_at=p1274_tmu_at;r->measured_at=p1274_measured_at;
    r->direct_mc=p1274_mc;r->peak_mc=p1274_temp_peak;r->lit_mc=hakios_exynos_tmu_zone_mc(1);
    r->gpu_mc=hakios_exynos_tmu_zone_mc(2);r->slope_mc=p1274_slope_mc;
    r->bp=p1274_big_peak;r->lp=p1274_lit_peak;
    r->ba=p1274_weight?(uint32_t)(p1274_big_weight/p1274_weight):0;
    r->la=p1274_weight?(uint32_t)(p1274_lit_weight/p1274_weight):0;
    r->bs=p1274_weight?(uint32_t)(p1274_big_sum_weight/p1274_weight):0;
    r->ls=p1274_weight?(uint32_t)(p1274_lit_sum_weight/p1274_weight):0;
    r->bsp=p1274_big_sum_peak;r->lsp=p1274_lit_sum_peak;
    r->bkhz=b->cur;r->lkhz=l->cur;r->bcap=ceiling_now(b);
    r->fast=__atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE);r->measured=b->measured;
    r->online=(b->online<<4)|l->online;r->recommended=hakios_p1274_recommended_mask();
    r->requested=__atomic_load_n(&p1274_requested_mask,__ATOMIC_ACQUIRE);
    int display=hakios_display_requested_on();r->display=display<0?2u:(unsigned)display;
    r->profile=p1274_active_profile(now);
    __atomic_store_n(&p1274_count,n+1,__ATOMIC_RELEASE);
    p1274_last_row=now;p1274_weight=p1274_big_weight=p1274_lit_weight=0;
    p1274_big_sum_weight=p1274_lit_sum_weight=0;
    p1274_big_peak=p1274_lit_peak=p1274_big_sum_peak=p1274_lit_sum_peak=0;
    p1274_temp_peak=-273000;
}
static void p1274_finish(unsigned reason,uint64_t now) {
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=2)return;
    if(p1274_last_row!=now)p1274_record(now,0,true);
    p1274_end=now;p1274_reason=reason;
    /* No further user events can enter once this deadline is cleared. */
    lck_spin_lock(&p1274_event_lock);
    __atomic_store_n(&hakios_p1274_deadline,0,__ATOMIC_SEQ_CST);
    lck_spin_unlock(&p1274_event_lock);
    hakios_p1273_trace_stop(reason,now);
    __atomic_store_n(&p1274_state,3,__ATOMIC_RELEASE);
}
static void p1274_tick(uint64_t now) {
    unsigned state=__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE);
    unsigned p=state==2?__atomic_load_n(&p1274_profile_id,__ATOMIC_ACQUIRE):0;
    p1274_fast_tick(now,p);
    if(state!=2)return;
    uint64_t dt=p1274_last_tick&&now>p1274_last_tick?now-p1274_last_tick:0;
    p1274_last_tick=now;
    unsigned reason=0;
    if(__atomic_load_n(&p1274_cancel,__ATOMIC_ACQUIRE))reason=6;
    else if(hakios_display_requested_on()!=0 || __atomic_load_n(&hakios_p1274_display_on_at,__ATOMIC_ACQUIRE))reason=4;
    else if(p1274_conflict() || (p>=P1274_REC2 && ocp_on))reason=8;
    else if(!s9_p1274_temp_valid(p1274_mc) || hakios_battery_temperature()<=0)reason=3;
    else if(p1274_mc>=76000)reason=2;
    else reason=p125_thermal_reason();
    if(!reason && p>=P1274_FAST && !hakios_p123_input_quiet(now))reason=7;
    if(!reason && p>=P1274_FAST && !hakios_p124_ufs_safe())reason=8;
    if(reason==2){
        /* Safety stop must not lift a lower bound into a hot burst. */
        uint32_t old=__atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE);
        uint32_t cap=old?MIN(old,1261000u):1261000u;
        __atomic_store_n(&p1274_fast_limit,cap,__ATOMIC_RELEASE);
        if(cap!=old)p1274_rate_event(1,now,old,cap,cap,0,0);
    }
    if(!reason && now>=p1274_planned_end)reason=1;
    /* Arm only near the historical late event. 10s of warm prehistory and
     * up to 32s rolling recount storage, not a six-minute high-res buffer. */
    if(!reason && now>=p1274_ticks_ms(230000) &&
       __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)==0){
        if(!hakios_p1273_trace_arm(now,p1274_planned_end,p1274_ticks_ms(250)))reason=8;
    }
    if(__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)==2){
        bool eligible=now>=p1274_ticks_ms(240000);
        if(eligible && clusters[1].load>=80){
            if(!p1274_high_since)p1274_high_since=now;
        }else p1274_high_since=0;
        unsigned bits=s9_p1274_trigger(eligible,p1274_high_since &&
            now-p1274_high_since>=p1274_ticks_ms(250),p1274_mc,p1274_rising);
        /* An early safety stop is retained, not mislabeled a late event. */
        if(reason==2)bits|=8u;
        uint64_t trigger=__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE);
        if(!trigger && bits){
            p1274_trigger_bits=bits;
            __atomic_store_n(&hakios_p1273_trace_trigger,now,__ATOMIC_RELEASE);
            trigger=now;
            uint64_t until=MIN(p1274_planned_end,now+p1274_ticks_ms(10000));
            __atomic_store_n(&hakios_p1273_trace_until,until,__ATOMIC_RELEASE);
        }
        uint64_t until=__atomic_load_n(&hakios_p1273_trace_until,__ATOMIC_ACQUIRE);
        if(now<until){p1273_gov_record(now);p1274_detail_record(now,dt);}
        if(reason || now>=until)hakios_p1273_trace_stop(reason?reason:(trigger?7:1),now);
    }
    if(now>=p1274_ticks_ms(240000)){
        /* Start only on the qualified event, not any isolated 40ms spike. */
        uint64_t triggered=__atomic_load_n(&hakios_p1273_trace_trigger,__ATOMIC_ACQUIRE);
        if(!p1274_burst_start && triggered)p1274_burst_start=p1274_high_since?p1274_high_since:triggered;
        if(p1274_burst_start && !p1274_burst_end){
            if(clusters[1].load<25 && clusters[0].load<25){
                if(!p1274_idle_since)p1274_idle_since=now;
                if(now-p1274_idle_since>=p1274_ticks_ms(3000))p1274_burst_end=p1274_idle_since;
            }else p1274_idle_since=0;
        }
    }
    p1274_record(now,dt,reason!=0);
    if(reason){p1274_pending_finish=reason;__atomic_store_n(&p1274_cancel,1,__ATOMIC_RELEASE);}
}
static int p1274_profile_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req) {
    int v=(int)p1274_active_profile(mach_absolute_time());
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v<0 || v>P1274_REC0)return EINVAL;
    if(!v){__atomic_store_n(&p1274_cancel,1,__ATOMIC_RELEASE);return 0;}
    uint64_t now=mach_absolute_time(),end=p1274_ticks_ms(420000);
    if(!p1274_boot_enabled() || now>=p1274_ticks_ms(225000) || p125_thermal_reason() ||
       hakios_battery_temperature()<=0 || p1274_conflict() || big_cores!=SCION_BIG_CORES ||
       __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=0)return EPERM;
    if(v>=P1274_FAST && (!hakios_p123_input_quiet(now) || !hakios_p124_ufs_safe()))return EPERM;
    if(v>=P1274_REC2 && ocp_on)return EPERM; /* never disable OCP to park scheduler cores */
    uint32_t cap=s9_p1274_cap(v);
    if(cap && !p1273_cap_rate_present(&clusters[1],cap))return EPERM;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&p1274_state,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    p1274_topology();
    if(__builtin_popcount(p1274_big_mask)!=4 || __builtin_popcount(p1274_lit_mask)!=4 ||
       (p1274_big_mask&p1274_lit_mask) || (p1274_big_mask|p1274_lit_mask)!=255u){
        __atomic_store_n(&p1274_state,0,__ATOMIC_RELEASE);return EPERM;
    }
    p1274_requested_mask=p1274_big_mask|p1274_lit_mask;
    p1274_start=now;p1274_planned_end=end;
    p1274_profile_id=(unsigned)v;p1274_cancel=0;p1274_reason=0;
    __atomic_store_n(&hakios_p1274_trace_late,1,__ATOMIC_RELEASE);
    __atomic_store_n(&hakios_p1274_deadline,end,__ATOMIC_RELEASE);
    __atomic_store_n(&p1274_state,2,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_profile,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_profile_sysctl,"I","P12.7.4 0 stop,1 baseline,2 log-only,3 fast,4-6 cap,7-9 recommended");
static int p1274_log_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req) {
    unsigned p=p1274_active_profile(mach_absolute_time());
    int mode=!p?-1:p==P1274_BASE?0:2;
    return SYSCTL_OUT(req,&mode,sizeof(mode));
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_metal_log_mode,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_log_sysctl,"I","P12.7.4 effective user observation/log mode -1 off,0 baseline,2 counters");
static int p1274_event_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req) {
    uint64_t v[8],now=mach_absolute_time(),tid=0,task=0;uint32_t pid=0;
    if(!req->newptr || req->newlen!=sizeof(v) || !p1274_active_profile(now))return EPERM;
    int e=SYSCTL_IN(req,v,sizeof(v));if(e)return e;
    if(v[0]<1 || v[0]>5)return EINVAL;
    hakios_p1273_identity(&tid,&task,&pid);
    if(!pid)return EPERM;
    lck_spin_lock(&p1274_event_lock);
    now=mach_absolute_time();
    if(!p1274_active_profile(now)){lck_spin_unlock(&p1274_event_lock);return EPERM;}
    /* Limit each reporting task to 32 events per second. A full table
     * or rejected event remains explicit missing evidence. */
    struct p1274_producer *prod=NULL;
    for(unsigned i=0;i<128;i++){
        struct p1274_producer *q=&p1274_producers[i];
        if(q->task==task || !q->task){prod=q;break;}
    }
    if(!prod || !task){p1274_event_drops++;lck_spin_unlock(&p1274_event_lock);return ENOSPC;}
    if(!prod->task || now-prod->at>=p1274_ticks_ms(1000)){prod->task=task;prod->at=now;prod->n=0;}
    if(prod->n>=32){p1274_event_drops++;lck_spin_unlock(&p1274_event_lock);return EBUSY;}
    prod->n++;
    unsigned n=p1274_event_count;
    if(n>=P1274_EVENTS){p1274_event_drops++;lck_spin_unlock(&p1274_event_lock);return ENOSPC;}
    struct p1274_event *r=&p1274_events[n];
    r->at=now;r->task=task;r->tid=tid;r->pid=pid;memcpy(r->v,v,sizeof(v));
    p1274_event_count=n+1;
    lck_spin_unlock(&p1274_event_lock);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_user_event,CTLTYPE_OPAQUE|CTLFLAG_WR|CTLFLAG_LOCKED|CTLFLAG_ANYBODY|CTLFLAG_KERN,NULL,0,p1274_event_sysctl,"S,p1274event","P12.7.4 bounded advisory user evidence, no policy");
static int p1274_status_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req) {
    unsigned state=__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE);
    char buf[448];
    if(state!=3){
        snprintf(buf,sizeof(buf),"version=1 state=%u profile=%u now=%llu",state,
            p1274_active_profile(mach_absolute_time()),(unsigned long long)mach_absolute_time());
    }else{
        if(__atomic_load_n(&hakios_p1274_writers,__ATOMIC_SEQ_CST))return EBUSY;
        snprintf(buf,sizeof(buf),"version=1 state=3 profile=%u start=%llu end=%llu planned=%llu reason=%u rows=%u fg=%llu events=%u drops=%u rate_events=%u rate_drops=%u bigmask=%x litmask=%x trigger_bits=%u burst_start=%llu burst_end=%llu measure_ok=%llu measure_bad=%llu measure_skip=%llu usb_down=%llu usb_up=%llu",
            p1274_profile_id,(unsigned long long)p1274_start,(unsigned long long)p1274_end,(unsigned long long)p1274_planned_end,p1274_reason,
            p1274_count,(unsigned long long)__atomic_load_n(&hakios_p1274_fg_seq,__ATOMIC_ACQUIRE),p1274_event_count,p1274_event_drops,p1274_rate_count,p1274_rate_drops,p1274_big_mask,p1274_lit_mask,p1274_trigger_bits,
            (unsigned long long)p1274_burst_start,(unsigned long long)p1274_burst_end,
            (unsigned long long)p1274_measure_ok,(unsigned long long)p1274_measure_bad,(unsigned long long)p1274_measure_skip,
            (unsigned long long)__atomic_load_n(&hakios_p1274_usb_down,__ATOMIC_ACQUIRE),(unsigned long long)__atomic_load_n(&hakios_p1274_usb_up,__ATOMIC_ACQUIRE));
    }
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_status,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_status_sysctl,"A","P12.7.4 frozen phase coverage");
static int p1274_restore_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[192];
    snprintf(buf,sizeof(buf),"version=1 state=%u profile=%u cores=%u actual=%x requested=%x fast=%u online=%u exit=%x display=%d",
      __atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE),p1274_active_profile(mach_absolute_time()),
      big_cores,hakios_p1274_recommended_mask(),__atomic_load_n(&p1274_requested_mask,__ATOMIC_ACQUIRE),
      __atomic_load_n(&p1274_fast_limit,__ATOMIC_ACQUIRE),(clusters[1].online<<4)|clusters[0].online,
      hakios_cpu_exit_mask,hakios_display_requested_on());
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_restore,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_restore_sysctl,"A","P12.7.4 policy and actual recommendation restore sample");
static int p1274_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p1274_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v<0 || v>5007)return EINVAL;
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=3)return EBUSY;
    __atomic_store_n(&p1274_select,(unsigned)v,__ATOMIC_RELEASE);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_select_sysctl,"I","P12.7.4 frozen select");
static int p1274_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    if(__atomic_load_n(&p1274_state,__ATOMIC_ACQUIRE)!=3)return EBUSY;
    if(__atomic_load_n(&hakios_p1274_writers,__ATOMIC_SEQ_CST))return EBUSY;
    unsigned s=__atomic_load_n(&p1274_select,__ATOMIC_ACQUIRE);char buf[448];int n=-1;
    if(s<p1274_count){
        const struct p1274_row *r=&p1274_rows[s];
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=1 at=%llu tmu_at=%llu measured_at=%llu direct=%d peak=%d lit=%d gpu=%d slope=%d bp=%u lp=%u ba=%u la=%u bs=%u ls=%u bsp=%u lsp=%u bkhz=%u lkhz=%u bcap=%u fast=%u measured=%u online=%u recommended=%x requested=%x display=%u profile=%u",s,
          (unsigned long long)r->at,(unsigned long long)r->tmu_at,(unsigned long long)r->measured_at,r->direct_mc,r->peak_mc,r->lit_mc,r->gpu_mc,r->slope_mc,r->bp,r->lp,r->ba,r->la,r->bs,r->ls,r->bsp,r->lsp,r->bkhz,r->lkhz,r->bcap,r->fast,r->measured,r->online,r->recommended,r->requested,r->display,r->profile);
    }else if(s>=512 && s<512+P1274_FG_SLOTS){
        unsigned i=s-512;const struct p127_fg_row *r=&hakios_p1274_fg_rows[i];
        uint64_t seq=__atomic_load_n(&r->seq,__ATOMIC_ACQUIRE);
        if(seq!=i+1)return ENOENT;
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=2 seq=%llu at=%llu ua=%d avg=%d uv=%u external=%u charger=%u qh=%u",s,
          (unsigned long long)seq,(unsigned long long)r->ticks,r->ua,r->avg_ua,r->uv,r->external,r->charger,r->qh);
    }else if(s>=1024 && s<1024+p1274_event_count){
        const struct p1274_event *r=&p1274_events[s-1024];
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=3 at=%llu pid=%u task=%llu tid=%llu event=%llu a=%llu b=%llu c=%llu d=%llu e=%llu f=%llu g=%llu",s,
          (unsigned long long)r->at,r->pid,(unsigned long long)r->task,(unsigned long long)r->tid,
          (unsigned long long)r->v[0],(unsigned long long)r->v[1],(unsigned long long)r->v[2],(unsigned long long)r->v[3],(unsigned long long)r->v[4],(unsigned long long)r->v[5],(unsigned long long)r->v[6],(unsigned long long)r->v[7]);
    }else if(s>=3072 && s<3072+p1274_rate_count){
        const struct p1274_rate_event *r=&p1274_rate_events[s-3072];
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=6 at=%llu began=%llu tmu_at=%llu type=%u from=%u to=%u cap=%u asked=%u error=%d mc=%d",s,
          (unsigned long long)r->at,(unsigned long long)r->began,(unsigned long long)r->tmu_at,r->type,r->from,r->to,r->cap,r->asked,r->error,r->mc);
    }else if(s>=4096 && s<4096+P1274_DETAIL){
        const struct p1274_detail *r=&p1274_detail[s-4096];uint64_t w=r->weight;
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=4 present=%u bin=%llu at=%llu tmu_at=%llu bp=%u lp=%u ba=%llu la=%llu bs=%llu ls=%llu bsp=%u lsp=%u bkhz=%u lkhz=%u direct=%d peak=%d",s,r->valid,
          (unsigned long long)r->bin,(unsigned long long)r->at,(unsigned long long)r->tmu_at,r->bp,r->lp,
          (unsigned long long)(w?r->bw/w:0),(unsigned long long)(w?r->lw/w:0),(unsigned long long)(w?r->bsw/w:0),(unsigned long long)(w?r->lsw/w:0),r->bsp,r->lsp,r->bkhz,r->lkhz,r->mc,r->peak_mc);
    }else if(s>=5000 && s<5008){
        unsigned i=s-5000;
        n=snprintf(buf,sizeof(buf),"version=1 select=%u kind=5 cpu=%u n=%llu ticks=%llu",s,i,
          (unsigned long long)__atomic_load_n(&hakios_p1274_wfi_n[i],__ATOMIC_ACQUIRE),
          (unsigned long long)__atomic_load_n(&hakios_p1274_wfi_ticks[i],__ATOMIC_ACQUIRE));
    }
    if(n<0 || (size_t)n>=sizeof(buf))return EINVAL;
    return SYSCTL_OUT(req,buf,(size_t)n+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1274_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1274_page_sysctl,"A","P12.7.4 immutable timeline/FG/event/detail/WFI rows");
#endif
