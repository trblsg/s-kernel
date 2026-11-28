/* P1277 auto-idle: independent, explicit boot opt-in. No interactive
 * qualification bits are fabricated or persisted. Existing P127 policy
 * lease drives exactly the same floor/ABOX/BASE consumers as Rev6.
 * All PMIC transactions run on the existing governor worker, not IRQs.
 * The auto mutex serializes its state; the shared gate spin lock serializes
 * lease publication against legacy gate owners. Never hold the spin lock
 * over PMIC I/O or frequency measurement. */
#ifndef HAKIOS_P1277_IDLE_H
#define HAKIOS_P1277_IDLE_H
static LCK_GRP_DECLARE(p1277_auto_group,"p1277-auto");
static LCK_MTX_DECLARE(p1277_auto_lock,&p1277_auto_group);
enum { P77_WAIT=0, P77_TRIAL=1, P77_RESTORE=2, P77_READY=3,
       P77_ACTIVE=4, P77_FAULT=5 };
static unsigned p1277_auto_state,p1277_auto_stage,p1277_auto_samples;
static unsigned p1277_auto_retries,p1277_auto_fault,p1277_auto_abox_ok;
static unsigned p1277_auto_last_uv,p1277_auto_last_mhz,p1277_auto_last_reason;
static uint64_t p1277_auto_quiet,p1277_auto_started,p1277_auto_sample_at;
static uint64_t p1277_auto_restore_until,p1277_auto_fault_mark;
static uint64_t p1277_auto_stop0,p1277_auto_resume0,p1277_auto_wait0;
static bool p1277_auto_table_ok,p1277_auto_stage_pass,p1277_auto_was_active;
static const unsigned p1277_auto_modes[]={17u,33u,49u,65u,5u};
static const uint32_t p1277_auto_rates[]={949000u,832000u,715000u,598000u};
static const uint32_t p1277_auto_uv[]={731250u,700000u,668750u,637500u};
extern bool hakios_p1277_panel_auto_compatible(void);

/* Cached thermal/ownership evidence only. Called with gate spin lock held
 * by p127_guard or from the governor. The old audit admission (60/59/60)
 * is NOT the idle admission. Retain its active safety ceiling. */
static unsigned p1277_auto_guard_reason(uint64_t lease){
    if(!__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE))return 12;
    uint64_t now=mach_absolute_time();
    if(now>=(lease&~(uint64_t)P127_MASK))return 1;
    if(hakios_display_requested_on()!=0)return 4;
    if(!hakios_p123_input_quiet(now))return 7;
    if(!p124_enabled || !gov_running || gov_policy!=2 || ocp_on ||
       clusters[0].online!=4 || clusters[1].online!=4 ||
       big_cores!=SCION_BIG_CORES || hakios_cpu_exit_mask ||
       !hakios_p124_ufs_safe() ||
       __atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p126_lease,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p124_cpu_until,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p12_cpu_mode,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&p8_big_cap_khz,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE))return 6;
    unsigned trace=__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE);
    unsigned cap=__atomic_load_n(&p1273_cap_state,__ATOMIC_ACQUIRE);
    if((trace!=0 && trace!=4) || cap==2 || cap==3 || __atomic_load_n(&hakios_p1273_gate_watch,__ATOMIC_ACQUIRE))return 6;
    int b=hakios_exynos_tmu_zone_mc(0),l=hakios_exynos_tmu_zone_mc(1);
    int g=hakios_exynos_tmu_zone_mc(2),bat=hakios_battery_temperature();
    if(b<0 || l<0 || g<0 || bat<=0 || hakios_exynos_tmu_age_ms()>2000)return 3;
    if(b>=67000 || l>=66000 || g>=68000 || bat>=45)return 2;
    if(hakios_p124_ufs_busy())return 8;
    if(clusters[0].sum_now+clusters[1].sum_now>=200u)return 8;
    if(hakios_p9_panel_auto_on() && !hakios_p1277_panel_auto_compatible())return 6;
    return 0;
}
/* Admission mutex is held by callers enabling automatic ownership.
 * Observe legacy recorder/gate state before allowing an automatic owner.
 * The worker never holds these spin locks across MMIO or PMIC requests. */
static bool p1277_auto_legacy_idle(void){
    lck_spin_lock(&p12752_lock);
    bool idle=p12752_idle_mode==0;
    lck_spin_unlock(&p12752_lock);
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(mach_absolute_time());
    idle=idle && !p1273_gate_phase && p1273_gate_idle();
    lck_spin_unlock(&p1273_gate_lock);
    return idle && !__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&hakios_p1273_offline_duration,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&hakios_p1273_offline_fixed,__ATOMIC_ACQUIRE);
}
static bool p1277_auto_table(void){
    uint32_t khz[LEVELS_MOST],uv[LEVELS_MOST];
    unsigned n=hakios_acpm_dvfs_table(2,khz,uv,LEVELS_MOST,say);
    if(!n || n>LEVELS_MOST)return false;
    /* Exact already-proven OPP/voltage pairs on this S9 variant. Refuse a
     * changed firmware table, not merely a nearby frequency or voltage. */
    for(unsigned j=0;j<4;j++){
        unsigned hits=0;
        for(unsigned i=0;i<n;i++)
            if(khz[i]==p1277_auto_rates[j] && uv[i]==p1277_auto_uv[j])hits++;
        if(hits!=1)return false;
    }
    return true;
}
static bool p1277_auto_abox_resumed(void){
    uint64_t s=__atomic_load_n(&hakios_p127_abox[0],__ATOMIC_ACQUIRE);
    uint64_t r=__atomic_load_n(&hakios_p127_abox[1],__ATOMIC_ACQUIRE);
    return s>=p1277_auto_stop0 && r>=p1277_auto_resume0 &&
           r-p1277_auto_resume0>=s-p1277_auto_stop0;
}
static void p1277_auto_fail(unsigned code){
    uint64_t lease=__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE);
    p1277_auto_fault=code;
    __atomic_store_n(&p1277_auto_enabled,0,__ATOMIC_RELEASE);
    if(lease)(void)hakios_p127_stop_owned(lease,6);
    /* If a live lease existed, still prove restore. A permanent failure
     * never rearms; an uncertain restore retains the ownership fence. */
    if(!lease)p1277_auto_state=P77_FAULT;
    say("p1277-auto: fault=%u stage=%u; automatic policy retired\n",code,p1277_auto_stage);
}
/* Layer 2 is a one-shot, independently qualified extension. No automatic
 * stacking of register owners, no new driver register programming here.
 * All transitions are serialized on the governor/auto mutex. */
enum { P77_L2_NONE=0,P77_L2_MFC=1,P77_L2_CAM=2,P77_L2_DISP200=3,P77_L2_DISP134=4 };
enum { P77_L2_IDLE=0,P77_L2_HELD=1,P77_L2_RESTORING=2 };
static unsigned p1277_l2_request,p1277_l2_module,p1277_l2_state,p1277_l2_result,p1277_l2_samples;
static uint64_t p1277_l2_until,p1277_l2_sample_at,p1277_l2_restore_until;
static bool p1277_l2_restored(void){
    return hakios_p545_mfc_restored() && hakios_p545_cam_restored() && hakios_p545_disp_restored();
}
static bool p1277_l2_qualified(unsigned m){
    switch(m){
    case P77_L2_MFC:return hakios_p127545_mfc_qualified();
    case P77_L2_CAM:return hakios_p127545_cam_qualified();
    case P77_L2_DISP200:return hakios_p545_disp_qualified(200000);
    case P77_L2_DISP134:return hakios_p545_disp_qualified(134000);
    default:return false;
    }
}
static bool p1277_l2_begin(unsigned m,uint64_t until){
    switch(m){
    case P77_L2_MFC:return hakios_p127545_mfc_begin(until);
    case P77_L2_CAM:return hakios_p127545_cam_begin(until);
    case P77_L2_DISP200:return hakios_p545_disp_begin(200000,until);
    case P77_L2_DISP134:return hakios_p545_disp_begin(134000,until);
    default:return false;
    }
}
static bool p1277_l2_live(unsigned m){
    switch(m){
    case P77_L2_MFC:return hakios_p127545_mfc_live();
    case P77_L2_CAM:return hakios_p127545_cam_live();
    case P77_L2_DISP200:case P77_L2_DISP134:return hakios_p545_disp_live();
    default:return false;
    }
}
static bool p1277_l2_end(unsigned m){
    switch(m){
    case P77_L2_MFC:return hakios_p127545_mfc_end();
    case P77_L2_CAM:return hakios_p127545_cam_end();
    case P77_L2_DISP200:case P77_L2_DISP134:return hakios_p545_disp_end();
    default:return false;
    }
}
/* Result: 1 clean bounded window; 2 aborted/preempted; 3 refused;
 * 4 uncertain restore. Driver end must succeed AND all fences clear
 * for result 1. Hardware watchdogs remain independently bounded. */
static bool p1277_l2_tick(uint64_t now,uint64_t lease){
    if(!p1277_l2_request && !p1277_l2_state)return true;
    bool live=lease && __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)==lease &&
        !p1277_auto_guard_reason(lease) && p1277_auto_state==P77_ACTIVE &&
        p1273_gate_faults()==p1277_auto_fault_mark;
    unsigned m=p1277_l2_state?p1277_l2_module:p1277_l2_request;
    /* Do not combine panel and a DISP clock owner in this revision. */
    if(m>=P77_L2_DISP200 && hakios_p9_panel_auto_on())live=false;
    if(p1277_l2_state==P77_L2_IDLE){
        p1277_l2_request=0;p1277_l2_module=m;p1277_l2_result=3;p1277_l2_samples=0;
        if(!live || !p1277_l2_restored() || !p1277_l2_qualified(m) ||
           (lease&~(uint64_t)P127_MASK)-now<p1274_ticks_ms(28000))return true;
        p1277_l2_until=now+p1274_ticks_ms(20000);
        p1277_l2_sample_at=now;
        p1277_l2_state=P77_L2_HELD;
        if(p1277_l2_begin(m,p1277_l2_until+p1274_ticks_ms(2000))){
            p1277_l2_result=0;
            /* Cancellation is distinguished from an active request. */
            p1277_l2_request=m;
        }else{
            p1277_l2_state=P77_L2_RESTORING;
            p1277_l2_restore_until=now+p1274_ticks_ms(5000);
        }
        return true;
    }
    if(p1277_l2_state==P77_L2_HELD){
        bool cancel=!p1277_l2_request || !live || p1277_l2_restored();
        bool complete=now>=p1277_l2_until;
        if(!cancel && now>=p1277_l2_sample_at){
            p1277_l2_sample_at=now+p1274_ticks_ms(1000);
            if(p1277_l2_live(m))p1277_l2_samples++;
            else cancel=true;
        }
        if(!cancel && !complete)return true;
        p1277_l2_result=2;p1277_l2_request=0;
        bool ended=!p1277_l2_restored() && p1277_l2_end(m);
        if(ended && !cancel && complete && p1277_l2_samples>=20 &&
           __atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)==lease &&
           !p1277_auto_guard_reason(lease))p1277_l2_result=1;
        p1277_l2_state=P77_L2_RESTORING;
        p1277_l2_restore_until=mach_absolute_time()+p1274_ticks_ms(5000);
    }
    if(!p1277_l2_restored()){
        (void)p1277_l2_end(m);
        if(!p1277_l2_restored()){
            if(mach_absolute_time()>=p1277_l2_restore_until){
                p1277_l2_result=4;p1277_auto_fail(6);p1277_auto_state=P77_FAULT;
            }else if(lease)(void)hakios_p127_stop_owned(lease,6);
            return false;
        }
    }
    p1277_l2_state=P77_L2_IDLE;p1277_l2_request=0;
    say("p1277-layer2: module=%u result=%u samples=%u restored=1\\n",m,p1277_l2_result,p1277_l2_samples);
    return true;
}
static int p1277_layer2_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    int v=(int)p1277_l2_request;lck_mtx_unlock(&p1277_auto_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v<0 || v>4)return EINVAL;
    P1277_ADMISSION(true,false);
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    uint64_t lease=__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE);
    if(v && (!p1277_auto_permission || p1277_auto_state!=P77_ACTIVE || !lease ||
       p1277_auto_guard_reason(lease) || p1277_l2_request || p1277_l2_state ||
       !p1277_l2_restored() || (v>=P77_L2_DISP200 && hakios_p9_panel_auto_on()) ||
       !p1277_l2_qualified((unsigned)v))){
        lck_mtx_unlock(&p1277_auto_lock);return EPERM;
    }
    p1277_l2_request=(unsigned)v;
    lck_mtx_unlock(&p1277_auto_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_layer2,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_layer2_sysctl,"I","Rev7 one-shot previously qualified module: 1 MFC 2 CAM 3 DISP200 4 DISP134; 0 cancel");
static int p1277_layer2_stats_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    char out[180];
    snprintf(out,sizeof(out),"version=1 request=%u module=%u state=%u result=%u samples=%u restored=%u power_qualified=0",
        p1277_l2_request,p1277_l2_module,p1277_l2_state,p1277_l2_result,p1277_l2_samples,p1277_l2_restored()?1u:0u);
    lck_mtx_unlock(&p1277_auto_lock);return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_layer2_stats,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_layer2_stats_sysctl,"A","Rev7 bounded extension and rollback witness, not power qualification");

static bool p1277_auto_start(uint64_t now,unsigned mode,bool active){
    uint64_t span=p1274_ticks_ms(active?120000u:20000u);
    uint64_t lease=((now+span)&~(uint64_t)P127_MASK)|mode,expect=0;
    if(!p1277_auto_legacy_idle() || p1277_auto_guard_reason(lease) || hakios_p124_ufs_busy() ||
       clusters[0].sum_now+clusters[1].sum_now>=150u ||
       (P127_FLOOR(mode) && !p127_floor_safe(mode)) ||
       !hakios_p545_mfc_restored() || !hakios_p545_cam_restored() ||
       !hakios_p545_disp_restored())return false;
    lck_spin_lock(&p1273_gate_lock);
    p1273_gate_refresh_locked(now);
    if(p1273_gate_phase || !p1273_gate_idle() || p1277_auto_guard_reason(lease)){
        lck_spin_unlock(&p1273_gate_lock);return false;
    }
    p1277_auto_stop0=__atomic_load_n(&hakios_p127_abox[0],__ATOMIC_ACQUIRE);
    p1277_auto_resume0=__atomic_load_n(&hakios_p127_abox[1],__ATOMIC_ACQUIRE);
    p1277_auto_wait0=__atomic_load_n(&hakios_p127_abox[2],__ATOMIC_ACQUIRE);
    __atomic_store_n(&p1277_auto_lease,lease,__ATOMIC_RELEASE);
    bool ok=__atomic_compare_exchange_n(&hakios_p127_lease,&expect,lease,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
    if(!ok)__atomic_store_n(&p1277_auto_lease,0,__ATOMIC_RELEASE);
    else __atomic_store_n(&hakios_p127_reason,0,__ATOMIC_RELEASE);
    lck_spin_unlock(&p1273_gate_lock);
    if(!ok)return false;
    p1277_auto_started=now;p1277_auto_sample_at=now+p1274_ticks_ms(3000);
    p1277_auto_samples=0;p1277_auto_stage_pass=false;p1277_auto_was_active=active;
    p1277_auto_state=active?P77_ACTIVE:P77_TRIAL;
    p1277_auto_quiet=0;
    unsigned why=p1277_auto_guard_reason(lease);
    if(why)(void)hakios_p127_stop_owned(lease,why);
    say("p1277-auto: %s stage=%u mode=%u lease=%llu\n",active?"active":"trial",
        p1277_auto_stage,mode,(unsigned long long)lease);
    return true;
}
/* One fresh frequency witness, using the same measurement helper as the
 * existing governor summary. Never rely on a requested clock alone. */
static bool p1277_auto_rate(uint32_t target,bool stock){
    uint32_t cur=clusters[0].cur;
    if(stock?cur<1053000u:cur!=target)return false;
    uint32_t m=measure_mhz(&clusters[0]);
    p1277_auto_last_mhz=m;
    return cur==clusters[0].cur && p1273_rate_matches(m,cur);
}
/* One explicit CPU burst trigger, using the existing bounded PID/TID
 * recount ring. Do not overwrite a frozen trace or invent pretrigger data.
 * Release and prove the automatic floor first; no trace overlaps a lease. */
static unsigned p1277_burst_request,p1277_burst_owned;
static uint64_t p1277_burst_since,p1277_burst_trigger,p1277_burst_until;
static void p1277_burst_cancel(uint64_t now){
    p1277_burst_request=0;p1277_burst_since=0;
    if(p1277_burst_owned && __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)==2)
        hakios_p1273_trace_stop(6,now);
}
static bool p1277_burst_tick(uint64_t now,uint64_t lease){
    if(!p1277_burst_request)return false;
    if(!__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE) || now>=p1277_burst_until ||
       __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=0 ||
       __atomic_load_n(&hakios_p1274_trace_late,__ATOMIC_ACQUIRE)){
        p1277_burst_request=0;p1277_burst_since=0;return false;
    }
    if(p125_thermal_reason() || hakios_display_requested_on()!=0 ||
       !hakios_p123_input_quiet(now) || !hakios_p124_ufs_safe() ||
       clusters[0].sum_now+clusters[1].sum_now<150u){
        p1277_burst_since=0;return false;
    }
    if(!p1277_burst_since)p1277_burst_since=now;
    if(now-p1277_burst_since<p1274_ticks_ms(250))return false;
    if(lease){
        (void)hakios_p127_stop_owned(lease,8);
        return false; /* run normal restore proof, never block it */
    }
    if(p1277_l2_state || !p1277_l2_restored() || !p1277_auto_legacy_idle())return true;
    if(hakios_p1273_trace_arm(now,now+p1274_ticks_ms(10000),p1274_ticks_ms(250))){
        uint64_t zero=0;
        (void)__atomic_compare_exchange_n(&hakios_p1273_trace_trigger,&zero,now,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        p1277_burst_owned=1;p1277_burst_trigger=now;
    }
    p1277_burst_request=0;p1277_burst_since=0;
    return true;
}
static int p1277_burst_trace_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    int v=(int)p1277_burst_request;lck_mtx_unlock(&p1277_auto_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v!=0 && v!=1)return EINVAL;
    P1277_ADMISSION(true,false);
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    if(v){
        if(!p1277_auto_permission || !__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE) ||
           p1277_auto_fault || p1277_burst_request || p1277_burst_owned ||
           __atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE)!=0 ||
           __atomic_load_n(&hakios_p1274_trace_late,__ATOMIC_ACQUIRE)){
            lck_mtx_unlock(&p1277_auto_lock);return EPERM;
        }
        p1277_burst_since=0;p1277_burst_request=1;
        p1277_burst_until=mach_absolute_time()+p1274_ticks_ms(120000);
    }else p1277_burst_cancel(mach_absolute_time());
    lck_mtx_unlock(&p1277_auto_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_burst_trace,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_burst_trace_sysctl,"I","Rev7 120s wait for one sustained OFF CPU burst, then 10s existing PID/TID recorder; 0 cancel");
static int p1277_burst_stats_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    char out[240];
    snprintf(out,sizeof(out),"version=1 request=%u owned=%u trace_state=%u observed_trigger=%llu trace_generation=%llu threshold=150 dwell_ms=250 pretrigger=0",
        p1277_burst_request,p1277_burst_owned,__atomic_load_n(&hakios_p1273_trace_state,__ATOMIC_ACQUIRE),
        (unsigned long long)p1277_burst_trigger,(unsigned long long)__atomic_load_n(&hakios_p1273_trace_generation,__ATOMIC_ACQUIRE));
    lck_mtx_unlock(&p1277_auto_lock);return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_burst_stats,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_burst_stats_sysctl,"A","Rev7 trigger witness; trace coverage and drops must be checked separately");

static void p1277_auto_tick_locked(uint64_t now){
    uint64_t lease=__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE);
    bool enabled=__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE)!=0;
    if(p1277_auto_state==P77_FAULT)return;
    if(!p1277_l2_tick(now,lease))return;
    if(p1277_burst_tick(now,lease))return;
    if(lease){
        uint64_t live=__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE);
        if(p1277_auto_state==P77_RESTORE){
            if(live){p1277_auto_fail(4);p1277_auto_state=P77_FAULT;return;}
            if(now<p1277_auto_sample_at)return;
            p1277_auto_sample_at=now+p1274_ticks_ms(1000);
            if(p1273_gate_faults()!=p1277_auto_fault_mark || !hakios_p124_ufs_safe()){
                p1277_auto_fail(4);p1277_auto_state=P77_FAULT;return;
            }
            if(__atomic_load_n(&hakios_p1273_last_stop_lease,__ATOMIC_ACQUIRE)==lease &&
               p1277_auto_rate(0,true) && p1277_auto_abox_resumed()){
                __atomic_store_n(&p1277_auto_lease,0,__ATOMIC_RELEASE);
                if(p1277_auto_fault){p1277_auto_state=P77_FAULT;return;}
                if(!p1277_auto_was_active){
                    if(p1277_auto_stage_pass && __atomic_load_n(&hakios_p1273_last_stop_reason,__ATOMIC_ACQUIRE)==0){
                        if(p1277_auto_stage==4)p1277_auto_abox_ok=1;
                        p1277_auto_stage++;p1277_auto_retries=0;
                    }else if(enabled && ++p1277_auto_retries>=3){
                        if(p1277_auto_stage==4){p1277_auto_stage=5;p1277_auto_abox_ok=0;}
                        else{p1277_auto_fail(3);return;}
                    }
                }
                p1277_auto_state=p1277_auto_stage>=5?P77_READY:P77_WAIT;
                p1277_auto_quiet=0;
                say("p1277-auto: restore verified stage=%u next=%u abox_driver_ok=%u\n",
                    p1277_auto_stage,p1277_auto_state,p1277_auto_abox_ok);
            }else if(now>=p1277_auto_restore_until){
                p1277_auto_fault=4;p1277_auto_state=P77_FAULT;
                __atomic_store_n(&p1277_auto_enabled,0,__ATOMIC_RELEASE);
                say("p1277-auto: restore uncertain; fence retained, reboot required\n");
            }
            return;
        }
        if(live!=lease){
            /* Never accept another owner's stop as our evidence. */
            if(live){p1277_auto_fail(5);p1277_auto_state=P77_FAULT;return;}
            /* A revoked trial cannot qualify, even if samples matched. */
            if(__atomic_load_n(&hakios_p1273_last_stop_lease,__ATOMIC_ACQUIRE)!=lease)p1277_auto_stage_pass=false;
            p1277_auto_last_reason=__atomic_load_n(&hakios_p1273_last_stop_reason,__ATOMIC_ACQUIRE);
            if(p1277_auto_last_reason!=0)p1277_auto_stage_pass=false;
            p1277_auto_state=P77_RESTORE;
            p1277_auto_sample_at=now+p1274_ticks_ms(2000);
            p1277_auto_restore_until=now+p1274_ticks_ms(15000);
            return;
        }
        if(p1273_gate_faults()!=p1277_auto_fault_mark || !hakios_p124_ufs_safe()){
            p1277_auto_fail(2);return;
        }
        unsigned why=p1277_auto_guard_reason(lease);
        if(why){(void)hakios_p127_stop_owned(lease,why);return;}
        if(p1277_auto_state==P77_ACTIVE){
            /* Refresh only the exact automatic lease after live checks.
             * Legacy trials remain non-renewable. */
            if((lease&~(uint64_t)P127_MASK)-now<p1274_ticks_ms(30000)){
                uint64_t next=((now+p1274_ticks_ms(120000))&~(uint64_t)P127_MASK)|(lease&P127_MASK),expect=lease;
                lck_spin_lock(&p1273_gate_lock);
                if(!p1277_auto_guard_reason(lease) &&
                   __atomic_compare_exchange_n(&hakios_p127_lease,&expect,next,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
                    __atomic_store_n(&p1277_auto_lease,next,__ATOMIC_RELEASE);
                lck_spin_unlock(&p1273_gate_lock);
            }
            return;
        }
        if(now<p1277_auto_sample_at)return;
        p1277_auto_sample_at=now+p1274_ticks_ms(3000);
        if(p1277_auto_stage<4){
            if(hakios_p124_ufs_busy() || !p127_floor_safe((unsigned)(lease&P127_MASK)))return;
            if(!p1277_auto_rate(p1277_auto_rates[p1277_auto_stage],false))return;
            uint8_t v=0;
            int error=hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC,0x23,&v);
            if(__atomic_load_n(&hakios_p127_lease,__ATOMIC_ACQUIRE)!=lease ||
               p1277_auto_guard_reason(lease) || hakios_p124_ufs_busy() ||
               clusters[0].cur!=p1277_auto_rates[p1277_auto_stage])return;
            p1277_auto_last_uv=error?0:300000u+6250u*v;
            if(error || p1277_auto_last_uv!=p1277_auto_uv[p1277_auto_stage]){
                p1277_auto_fail(1);return;
            }
            if(++p1277_auto_samples>=2)p1277_auto_stage_pass=true;
        }else{
            uint64_t s=__atomic_load_n(&hakios_p127_abox[0],__ATOMIC_ACQUIRE);
            uint64_t w=__atomic_load_n(&hakios_p127_abox[2],__ATOMIC_ACQUIRE);
            if(s>p1277_auto_stop0 && w>p1277_auto_wait0)p1277_auto_stage_pass=true;
        }
        if(p1277_auto_stage_pass && !hakios_p127_stop_owned(lease,0))p1277_auto_stage_pass=false;
        return;
    }
    if(!enabled)return;
    if(p1277_auto_table_ok && (p1273_gate_faults()!=p1277_auto_fault_mark || !hakios_p124_ufs_safe())){
        p1277_auto_fail(2);return;
    }
    unsigned mode=p1277_auto_stage<5?p1277_auto_modes[p1277_auto_stage]:
        (p1277_auto_abox_ok?P127545_COMPOSITE:65u);
    uint64_t hypothetical=((now+p1274_ticks_ms(20000))&~(uint64_t)P127_MASK)|mode;
    if(!p12754_optin() || !p1273_gate_enabled() || p1277_auto_guard_reason(hypothetical) ||
       !p1273_gate_idle() || hakios_p124_ufs_busy() || !__atomic_load_n(&touch_last,__ATOMIC_ACQUIRE) ||
       clusters[0].sum_now+clusters[1].sum_now>=150u ||
       (P127_FLOOR(mode) && !p127_floor_safe(mode))){
        p1277_auto_quiet=0;return;
    }
    if(!p1277_auto_quiet){p1277_auto_quiet=now;return;}
    if(now-p1277_auto_quiet<p1274_ticks_ms(3000))return;
    if(!p1277_auto_table_ok){
        if(!p1277_auto_table()){p1277_auto_fail(1);return;}
        p1277_auto_table_ok=true;p1277_auto_fault_mark=p1273_gate_faults();
    }
    (void)p1277_auto_start(now,mode,p1277_auto_stage>=5);
}
static void p1277_auto_tick(uint64_t now){
    if(!p1277_auto_blocks())return;
    if(!lck_mtx_try_lock(&p1277_auto_lock))return;
    p1277_auto_tick_locked(now);
    lck_mtx_unlock(&p1277_auto_lock);
}
static int p1277_auto_idle_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v!=0 && v!=1)return EINVAL;
    if(!p1277_auto_permission)return ENODEV;
    P1277_ADMISSION(true,false);
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    if(v){
        if(p1277_auto_fault || !p12754_optin() || !p1273_gate_enabled() ||
           __atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE)){
            lck_mtx_unlock(&p1277_auto_lock);return EPERM;
        }
        if(!__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE)){
            bool ok=p1277_auto_legacy_idle() && !__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE);
            if(ok)__atomic_store_n(&p1277_auto_enabled,1,__ATOMIC_RELEASE);
            if(!ok){lck_mtx_unlock(&p1277_auto_lock);return EBUSY;}
        }
    }else{
        p1277_burst_cancel(mach_absolute_time());
        p1277_l2_request=0;
        __atomic_store_n(&p1277_auto_enabled,0,__ATOMIC_RELEASE);
        uint64_t lease=__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE);
        if(lease)(void)hakios_p127_stop_owned(lease,12);
        p1277_auto_quiet=0;
    }
    lck_mtx_unlock(&p1277_auto_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_auto_idle,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_auto_idle_sysctl,"I","Rev7 explicit auto-idle owner, 0 stop 1 enable if boot permission");
static int p1277_auto_stats_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    if(!lck_mtx_try_lock(&p1277_auto_lock))return EBUSY;
    char out[320];
    snprintf(out,sizeof(out),"version=1 permission=%u enabled=%u state=%u stage=%u samples=%u retries=%u abox_driver_ok=%u fault=%u last_uv=%u last_mhz=%u last_reason=%u own_lease=%llu live_mode=%u ufs_safe=%u interactive_qualification=0",
        p1277_auto_permission,__atomic_load_n(&p1277_auto_enabled,__ATOMIC_ACQUIRE),
        p1277_auto_state,p1277_auto_stage,p1277_auto_samples,p1277_auto_retries,
        p1277_auto_abox_ok,p1277_auto_fault,p1277_auto_last_uv,p1277_auto_last_mhz,p1277_auto_last_reason,
        (unsigned long long)__atomic_load_n(&p1277_auto_lease,__ATOMIC_ACQUIRE),
        hakios_p127_mode(),hakios_p124_ufs_safe()?1u:0u);
    lck_mtx_unlock(&p1277_auto_lock);return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_auto_stats,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_auto_stats_sysctl,"A","Rev7 automatic driver/clock witness, not functional audio or power qualification");

/* Active thermal is a lower-only boost profile. CPU thermal governor,
 * frequency floors, saturated-load response and frame floor stay intact. */
static int p1277_active_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p1277_active_enabled,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v!=0 && v!=1)return EINVAL;
    if(v && !p1277_active_permission)return ENODEV;
    P1277_ADMISSION(true,false);
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE))return EBUSY;
    __atomic_store_n(&p1277_active_enabled,(unsigned)v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_active,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_active_sysctl,"I","Rev7 lower-only active CPU boost profile; GPU IRQ independently controlled");
/* Permission only at boot; persistent G2 is separate from the short
 * P127542 trial. The renderer retains its context-local fault latch. */
static uint32_t p1277_g2_request;
static int p1277_g2_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=__atomic_load_n(&p1277_g2_request,__ATOMIC_ACQUIRE) && p127542_g2_safe();
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v!=0 && v!=1)return EINVAL;
    P1277_ADMISSION(true,false);
    if(v && (!p1277_g2_permission || !p127542_g2_safe()))return EPERM;
    __atomic_store_n(&p1277_g2_request,(unsigned)v,__ATOMIC_RELEASE);
    return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_g2,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_g2_sysctl,"I","Rev7 verified G2 context opt-in, active only on safe display-on path");
/* Query only. Conduit may be intercepted by Carbon/EL2. Never claim
 * these results prove EL3 execution or a valid XNU resume path. */
static uint32_t p1277_psci_done,p1277_psci_version,p1277_psci_features;
#if defined(__arm64__) || defined(__aarch64__)
static uint32_t p1277_psci_read(uint32_t id,uint32_t arg){
    /* Whitelist both read-only function IDs even at the helper boundary. */
    if((id!=0x84000000u || arg!=0) && (id!=0x8400000au || arg!=0xc4000001u))return UINT32_MAX;
    register uint64_t x0 __asm__("x0")=id,x1 __asm__("x1")=arg;
    register uint64_t x2 __asm__("x2")=0,x3 __asm__("x3")=0;
    __asm__ volatile("smc #0":"+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3)::
        "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    return (uint32_t)x0;
}
#endif
static void p1277_psci_query_once(void){
    if(!p1277_psci_permission || __atomic_load_n(&p1277_psci_done,__ATOMIC_ACQUIRE))return;
    uint32_t v=UINT32_MAX,f=UINT32_MAX;
#if defined(__arm64__) || defined(__aarch64__)
    v=p1277_psci_read(0x84000000u,0);
    if((int32_t)v>=0 && (v>>16)>=1)f=p1277_psci_read(0x8400000au,0xc4000001u);
#endif
    p1277_psci_version=v;p1277_psci_features=f;
    __atomic_store_n(&p1277_psci_done,1,__ATOMIC_RELEASE);
    say("p1277-psci: conduit=SMC_EL2_possible version=0x%08x features=0x%08x signed=%d suspend_called=0\n",v,f,(int32_t)f);
}
static int p1277_psci_stats_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    unsigned done=__atomic_load_n(&p1277_psci_done,__ATOMIC_ACQUIRE);
    uint32_t v=done?p1277_psci_version:UINT32_MAX,f=done?p1277_psci_features:UINT32_MAX;
    char out[240];
    snprintf(out,sizeof(out),"version=1 permission=%u done=%u raw_version=0x%08x raw_features=0x%08x signed_features=%d osi=%u extended=%u conduit=SMC_EL2_possible suspend_called=0",
        p1277_psci_permission,done,v,f,(int32_t)f,(int32_t)f>=0?(f&1u):0u,(int32_t)f>=0?((f>>1)&1u):0u);
    return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_psci_stats,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_psci_stats_sysctl,"A","Read-only PSCI capability query; no CPU suspend");
static int p1277_revision_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=7;return SYSCTL_OUT(req,&v,sizeof(v));
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p1277_revision,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p1277_revision_sysctl,"I","P1277 implementation revision; audit ABI unchanged");
#endif
