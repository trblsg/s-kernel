/* P127545 DISP lab, included by hakios.exynos_acpm.c after bus metadata.
 * Default OFF, exact FVMAP entries and stable selected DPU path required.
 * This is a candidate ACPM domain transition, not full virtual OPP proof.
 * No raw CMU writes; only domain8 FREQ_REQ under acpm_lock. The gate is
 * reopened ONLY after verified stock path and trigger restoration.
 */
extern int hakios_display_requested_on(void);
extern bool hakios_p545_disp_dpu_quiesce(void);
extern bool hakios_p545_disp_dpu_restore(void);
extern bool hakios_p545_disp_panel_quiet(void);
extern uint32_t hakios_p545_disp_frame_seq(void);
extern bool hakios_p545_disp_frame_review(uint32_t seq);

static vm_offset_t p545_disp_top,p545_disp_mif,p545_disp_pmu;
static uint32_t p545_disp_base[P545_NREG],p545_disp_live_raw[P545_NREG];
static uint32_t p545_disp_target,p545_disp_seen,p545_disp_restored,p545_disp_qual,p545_disp_pending,p545_disp_review_seq;
static uint32_t p545_disp_attempts,p545_disp_sets,p545_disp_restores,p545_disp_preempt,p545_disp_fault,p545_disp_reviews,p545_disp_last_review;
static uint32_t p545_disp_samples,p545_disp_min,p545_disp_max;
static uint64_t p545_disp_start,p545_disp_end,p545_disp_until;
static bool p545_disp_active,p545_disp_touched;

static bool p545_disp_optin(void){
    uint32_t v=0,a=0,b=0;
    return PE_parse_boot_argn("p127545_disp",&v,sizeof(v)) && v==1 &&
        PE_parse_boot_argn("p12753",&a,sizeof(a)) && a==1 &&
        PE_parse_boot_argn("p12754",&b,sizeof(b)) && b==1;
}
static bool p545_disp_domain_on(void){
    return p545_disp_pmu && (rd(p545_disp_pmu,EXYNOS_CAM_PMU_DPU_STATUS)&15u)==15u;
}
static bool p545_disp_snap(uint32_t r[P545_NREG],struct p545_cmu_rates *rates){
    uint32_t again[P545_NREG];
    if(!p545_disp_top)p545_disp_top=ml_io_map(EXYNOS_CMU_TOP_PA,0x4000);
    if(!p545_disp_mif)p545_disp_mif=ml_io_map(CMU_MIF_PA,0x2000);
    if(!p545_disp_pmu)p545_disp_pmu=ml_io_map(EXYNOS_PMU_PA,0x5000);
    if(!p545_disp_top || !p545_disp_mif || !p545_disp_domain_on())return false;
    for(unsigned i=0;i<P545_NREG;i++)r[i]=rd(i>=P545_MIF_PLL?p545_disp_mif:p545_disp_top,p545_offsets[i]);
    __builtin_arm_dsb(15);
    for(unsigned i=0;i<P545_NREG;i++)again[i]=rd(i>=P545_MIF_PLL?p545_disp_mif:p545_disp_top,p545_offsets[i]);
    if(memcmp(r,again,sizeof(again)))return false;
    *rates=p545_decode(r);
    return rates->disp!=0;
}
static bool p545_disp_rate_match(uint32_t rate,uint32_t want){
    uint32_t diff=rate>want?rate-want:want-rate;
    return rate && diff<=want/100u; /* 133333 is 134000's nominal OPP */
}
static bool p545_disp_same_path(const uint32_t *a,const uint32_t *b){
    if(a[P545_DPU_M]!=b[P545_DPU_M])return false;
    if(!(a[P545_DPU_M]&1u))
        return a[P545_PLL0]==b[P545_PLL0] && a[P545_D02]==b[P545_D02] && a[P545_DPU_D]==b[P545_DPU_D];
    if(a[P545_DPU_ALT_M]!=b[P545_DPU_ALT_M] || a[P545_DPU_ALT_D]!=b[P545_DPU_ALT_D])return false;
    switch(a[P545_DPU_ALT_M]&3u){
    case 0:return a[P545_PLL3]==b[P545_PLL3];
    case 1:return a[P545_PLL4]==b[P545_PLL4];
    case 2:return a[P545_PLL1]==b[P545_PLL1] && a[P545_D13]==b[P545_D13];
    default:return a[P545_PLL2]==b[P545_PLL2] && a[P545_D22]==b[P545_D22];
    }
}
static bool p545_disp_others_match(struct p545_cmu_rates r){
    struct p545_cmu_rates b=p545_decode(p545_disp_base);
    return b.busc && b.bus1 && b.mif_pll && b.ddrphy2x && b.fsys0 && r.busc==b.busc && r.bus1==b.bus1 && r.mif_pll==b.mif_pll && r.ddrphy2x==b.ddrphy2x && r.fsys0==b.fsys0;
}
/* Verify the actual firmware's known five DISP entries and boot index.
 * Boot metadata alone never admits a write: live path is checked too. */
static bool p545_disp_table(void){
    const uint32_t freq[]={640000,534000,400000,200000,134000};
    uint32_t map=fvmap_at(printf),h=map+8u*FV_HEADER;
    if(!map || !sram_ok(h,FV_HEADER) || srd8(h+FV_NUM_LV)!=5u || srd8(h+8)!=2u)return false;
    uint32_t at=map+srd16(h+FV_O_RATEVOLT);
    if(!sram_ok(at,5u*8u))return false;
    for(unsigned i=0;i<5;i++){
        uint32_t uv=srd(at+i*8u+4u);
        if(srd(at+i*8u)!=freq[i] || uv<300000 || uv>1500000)return false;
    }
    return true;
}
static bool p545_disp_set_locked(uint32_t want){
    if(!p545_disp_closed() || !acpm_ready || !channels[HAKIOS_ACPM_CH_DVFS].usable ||
       (want!=400000 && want!=200000 && want!=134000))return false;
    uint32_t cmd[4]={8,want,0,0};
    bool submitted=false;
    bool ok=p545_dvfs_once(cmd,&submitted);
    if(submitted){p545_disp_sets++;p545_disp_touched=true;}
    return ok;
}
static bool p545_disp_wait_rate(uint32_t want,bool stock){
    struct p545_cmu_rates r;
    for(unsigned i=0;i<50;i++){
        if(p545_disp_snap(p545_disp_live_raw,&r) && p545_disp_rate_match(r.disp,want) &&
           (stock?p545_disp_same_path(p545_disp_base,p545_disp_live_raw):p545_disp_others_match(r))){
            if(stock)p545_disp_restored=r.disp;else p545_disp_seen=r.disp;
            return true;
        }
        delay(1000);
    }
    return false;
}
static bool p545_disp_restore_locked(bool preempt){
    if(!p545_disp_active)return false;
    if(preempt){p545_disp_preempt++;p545_disp_qual=p545_disp_pending=p545_disp_last_review=0;}
    /* Even an unacknowledged lower SET can have reached the APM.
     * Restore via the same serialized channel, then exact live path.
     * Never let a DECON/DSIM access through on an uncertain rollback. */
    bool ok=(!p545_disp_touched || p545_disp_set_locked(400000)) &&
        p545_disp_wait_rate(400000,true) && p545_disp_domain_on() &&
        hakios_p545_disp_dpu_restore();
    p545_disp_end=mach_absolute_time();p545_disp_active=false;p545_disp_touched=false;
    if(ok){
        p545_disp_restores++;p545_disp_review_seq=hakios_p545_disp_frame_seq();
        p545_disp_open();
    }else{
        p545_disp_fault++;p545_disp_qual=p545_disp_pending=p545_disp_last_review=0;
        printf("p127545-disp: rollback not verified; display MMIO stays blocked, recovery/reboot required\n");
    }
    return ok;
}
static void p545_disp_demand_worker(__unused thread_call_param_t a,__unused thread_call_param_t b){
    lck_mtx_lock(&acpm_lock);
    if(p545_disp_active && p545_disp_demand())(void)p545_disp_restore_locked(true);
    lck_mtx_unlock(&acpm_lock);
}
static void p545_disp_expiry_worker(__unused thread_call_param_t a,__unused thread_call_param_t b){
    lck_mtx_lock(&acpm_lock);
    if(p545_disp_active && mach_absolute_time()>=p545_disp_until)(void)p545_disp_restore_locked(true);
    lck_mtx_unlock(&acpm_lock);
}
static bool p545_disp_begin_locked(uint32_t want,uint64_t until,bool pilot){
    struct p545_cmu_rates r;
    if(!p545_disp_optin() || bus_max || !acpm_ready || p545_dvfs_uncertain ||
       !channels[HAKIOS_ACPM_CH_DVFS].usable || channels[HAKIOS_ACPM_CH_DVFS].buffer ||
       p545_disp_fault || p545_disp_pending ||
       p545_disp_active || p545_disp_closed() || hakios_display_requested_on()!=0 ||
       (want!=400000 && want!=200000 && want!=134000) || (pilot && want==400000) ||
       (want==134000 && !(p545_disp_qual&1u)) ||
       (!pilot && !(p545_disp_qual&(want==134000?2u:1u))))return false;
    if(!p545_disp_demand_call)p545_disp_demand_call=thread_call_allocate(p545_disp_demand_worker,NULL);
    if(!p545_disp_expiry_call)p545_disp_expiry_call=thread_call_allocate(p545_disp_expiry_worker,NULL);
    if(!p545_disp_demand_call || !p545_disp_expiry_call || !p545_disp_close())return false;
    if(!p545_disp_table() || !p545_disp_snap(p545_disp_base,&r) || !p545_disp_rate_match(r.disp,400000) || !p545_disp_others_match(r) ||
       hakios_display_requested_on()!=0 || p545_disp_demand() || !hakios_p545_disp_panel_quiet()){
        p545_disp_open();return false;
    }
    /* No MMIO reader remains. Quiesce at stock and verify DMA idle. */
    p545_disp_target=want;p545_disp_seen=0;p545_disp_restored=0;p545_disp_attempts++;
    p545_disp_samples=p545_disp_max=0;p545_disp_min=UINT32_MAX;
    p545_disp_active=true;p545_disp_touched=false;p545_disp_start=mach_absolute_time();p545_disp_until=until;
    bool ok=hakios_p545_disp_dpu_quiesce() && hakios_display_requested_on()==0 && !p545_disp_demand();
    if(ok && want!=400000){
        ok=p545_disp_set_locked(200000) && p545_disp_wait_rate(200000,false);
        if(ok && want==134000 && !p545_disp_demand() && hakios_display_requested_on()==0)
            ok=p545_disp_set_locked(134000) && p545_disp_wait_rate(134000,false);
        else if(want==134000)ok=false;
    }else if(ok)p545_disp_seen=r.disp;
    if(!ok || p545_disp_demand() || hakios_display_requested_on()!=0 || mach_absolute_time()>=until){
        (void)p545_disp_restore_locked(true);return false;
    }
    thread_call_enter_delayed(p545_disp_expiry_call,until);
    return true;
}
bool hakios_p545_disp_restored(void);
bool hakios_p545_disp_restored(void){return !p545_disp_closed();}
bool hakios_p545_disp_wake(void);
bool hakios_p545_disp_wake(void){
    if(!p545_disp_closed())return true;
    lck_mtx_lock(&acpm_lock);
    bool ok=!p545_disp_closed() || (p545_disp_active && p545_disp_restore_locked(true));
    lck_mtx_unlock(&acpm_lock);return ok;
}
bool hakios_p545_disp_probe(uint32_t want);
bool hakios_p545_disp_begin(uint32_t want,uint64_t until);
bool hakios_p545_disp_live(void);
bool hakios_p545_disp_end(void);
bool hakios_p545_disp_qualified(uint32_t want);
void hakios_p545_disp_invalidate(void);
bool hakios_p545_disp_probe(uint32_t want){
    if(!lck_mtx_try_lock(&acpm_lock))return false;
    uint64_t until=deadline_us(5000000);
    bool ok=p545_disp_begin_locked(want,until,true);
    if(ok){
        delay(20000);
        bool clean=!p545_disp_demand() && hakios_display_requested_on()==0;
        ok=p545_disp_restore_locked(!clean) && clean;
        if(ok)p545_disp_pending=want;
    }
    lck_mtx_unlock(&acpm_lock);return ok;
}
bool hakios_p545_disp_begin(uint32_t want,uint64_t until){
    if(!lck_mtx_try_lock(&acpm_lock))return false;
    bool ok=p545_disp_begin_locked(want,until,false);
    lck_mtx_unlock(&acpm_lock);return ok;
}
bool hakios_p545_disp_live(void){
    if(!lck_mtx_try_lock(&acpm_lock))return false;
    struct p545_cmu_rates r;
    bool ok=p545_disp_active && !p545_disp_demand() && hakios_display_requested_on()==0 &&
        p545_disp_snap(p545_disp_live_raw,&r) && p545_disp_rate_match(r.disp,p545_disp_target) &&
        p545_disp_others_match(r);
    if(ok){p545_disp_samples++;if(r.disp<p545_disp_min)p545_disp_min=r.disp;if(r.disp>p545_disp_max)p545_disp_max=r.disp;}
    else if(p545_disp_active)(void)p545_disp_restore_locked(true);
    lck_mtx_unlock(&acpm_lock);return ok;
}
bool hakios_p545_disp_end(void){
    lck_mtx_lock(&acpm_lock);
    bool clean=!p545_disp_demand() && hakios_display_requested_on()==0;
    bool ok=p545_disp_restore_locked(!clean) && clean;
    lck_mtx_unlock(&acpm_lock);return ok;
}
bool hakios_p545_disp_qualified(uint32_t want){
    if(!lck_mtx_try_lock(&acpm_lock))return false;
    bool ok=!p545_dvfs_uncertain && !p545_disp_fault && !p545_disp_pending && (p545_disp_qual&(want==134000?2u:1u));
    lck_mtx_unlock(&acpm_lock);return ok;
}
void hakios_p545_disp_invalidate(void){
    lck_mtx_lock(&acpm_lock);p545_disp_pending=p545_disp_qual=p545_disp_last_review=0;lck_mtx_unlock(&acpm_lock);
}
static int p545_disp_review_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    lck_mtx_lock(&acpm_lock);int v=(int)p545_disp_last_review;lck_mtx_unlock(&acpm_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);
    if(e || !req->newptr)return e;
    if(v!=0 && v!=200000 && v!=134000)return EINVAL;
    lck_mtx_lock(&acpm_lock);
    if(!v){p545_disp_pending=p545_disp_qual=p545_disp_last_review=0;lck_mtx_unlock(&acpm_lock);return 0;}
    uint64_t ttl,now=mach_absolute_time();nanoseconds_to_absolutetime(120ull*NSEC_PER_SEC,&ttl);
    bool ok=p545_disp_pending==(uint32_t)v && !p545_disp_closed() && !p545_disp_fault && !p545_dvfs_uncertain &&
        now>=p545_disp_end && now-p545_disp_end<ttl &&
        hakios_display_requested_on()==1 && hakios_p545_disp_frame_review(p545_disp_review_seq);
    if(ok){p545_disp_qual|=v==200000?1u:2u;p545_disp_pending=0;p545_disp_last_review=(uint32_t)v;p545_disp_reviews++;}
    lck_mtx_unlock(&acpm_lock);return ok?0:EBUSY;
}
static int p545_disp_stats_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    char out[480];
    lck_mtx_lock(&acpm_lock);
    snprintf(out,sizeof(out),"version=1 enabled=%u transport_ok=%u closed=%u active=%u target=%u seen=%u restored=%u qual=%u pending=%u attempts=%u sets=%u restores=%u preempt=%u fault=%u reviews=%u live_n=%u live_min=%u live_max=%u start=%llu end=%llu",
        p545_disp_optin(),!p545_dvfs_uncertain && acpm_ready && channels[HAKIOS_ACPM_CH_DVFS].usable && !channels[HAKIOS_ACPM_CH_DVFS].buffer,p545_disp_closed(),p545_disp_active,p545_disp_target,p545_disp_seen,p545_disp_restored,p545_disp_qual,p545_disp_pending,
        p545_disp_attempts,p545_disp_sets,p545_disp_restores,p545_disp_preempt,p545_disp_fault,p545_disp_reviews,p545_disp_samples,p545_disp_samples?p545_disp_min:0,p545_disp_max,
        (unsigned long long)p545_disp_start,(unsigned long long)p545_disp_end);
    lck_mtx_unlock(&acpm_lock);return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127545_disp_stats,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p545_disp_stats_sysctl,"A","Isolated DISP candidate, selected clock and rollback witness");
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127545_disp_review,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p545_disp_review_sysctl,"I","Approve same-boot DISP pilot after actual new scanout and visual review; 0 revoke");
