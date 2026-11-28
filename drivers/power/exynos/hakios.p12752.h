/* P12.7.5.2. Included after the P1274/P12751 lab.
 * One explicit quiet-window recorder, no bus/rail writes or power-state calls.
 * P125 owns the timer/FG interval; P127/P126 own their independent leases.
 * Governor is the only row writer. Separate phase identity prevents reusing
 * stale observations after the next A/B/A arm. */
#ifndef HAKIOS_P12752_H
#define HAKIOS_P12752_H
#define P12752_IDLE_ROWS 96u
extern uint32_t hakios_scanout_refreshes(void);
struct p12752_idle_row {
    uint64_t at,latency;
    uint32_t kind,index,raw,valid,bs,ls,bkhz,lkhz,bmeas,lmeas,age,policy,driver,refresh;
    int32_t error,big,lit,gpu,bat;
};
static struct p12752_idle_row p12752_idle_rows[P12752_IDLE_ROWS];
static LCK_GRP_DECLARE(p12752_group,"p12752-idle");
static LCK_SPIN_DECLARE(p12752_lock,&p12752_group);
static unsigned p12752_idle_mode,p12752_idle_count,p12752_idle_drops,p12752_idle_select,p12752_idle_reason,p12752_bus_next;
static uint64_t p12752_idle_until,p12752_idle_phase,p12752_idle_last,p12752_bus_last,p12752_idle_busy,p12752_guard_last;
static int p12752_revision=1;
SYSCTL_INT(_kern_hakios,OID_AUTO,p12752_revision,CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,&p12752_revision,0,"P12752 host revision");
static int p12752_profile_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p12752_selected,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    if(v<0 || v>4)return EINVAL;
    if(!v)return p1274_arm_profile(0,0);
    if(!p12752_boot_enabled() || (__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE)<0 || (__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE)&3)!=3) ||
       hakios_display_requested_on()!=0)return EPERM;
    /* 1=matched LOG2 baseline; 2/3/4=REC2/1/0. Stock arm retains OCP/UFS,
     * topology and input interlocks. Keep EarlyLoad and paired ICD evidence. */
    return p1274_arm_profile(v==1?P1274_LOG2:P1274_REC2+v-2,4u+(unsigned)v*256u);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_profile,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_profile_sysctl,"I","P12752 1 baseline 2 REC2 3 REC1 4 REC0");
static int p12752_eligibility_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[256];uint64_t now=mach_absolute_time();
    snprintf(buf,sizeof(buf),"version=1 enabled=%u conflict=%u ocp=%u ufs_safe=%u ufs_busy=%u quiet=%u thermal=%u cores=%u online=%u exit=%x",
        p12752_boot_enabled()?1u:0u,p1274_conflict()?1u:0u,ocp_on?1u:0u,
        hakios_p124_ufs_safe()?1u:0u,hakios_p124_ufs_busy()?1u:0u,hakios_p123_input_quiet(now)?1u:0u,
        p125_thermal_reason(),big_cores,(unsigned)(clusters[1].online*16+clusters[0].online),(unsigned)hakios_cpu_exit_mask);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_eligibility,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_eligibility_sysctl,"A","P12752 scheduler eligibility; never disables OCP");
static int p12752_sample_window_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    uint64_t now=mach_absolute_time();int v=0;
    if(p12752_boot_enabled() && __atomic_load_n(&p12752_selected,__ATOMIC_ACQUIRE) &&
       p1274_active_profile(now) && now>=p1274_ticks_ms(290000) && now<p1274_ticks_ms(390000) &&
       clusters[0].sum_now+clusters[1].sum_now>=150u)v=1;
    return SYSCTL_OUT(req,&v,sizeof(v));
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_sample_window,CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_sample_window_sysctl,"I","P12752 bounded natural busy-window user observation");
static int p12752_idle_mode_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v; lck_spin_lock(&p12752_lock);v=(int)p12752_idle_mode;lck_spin_unlock(&p12752_lock);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    if(v<0 || v>2)return EINVAL;
    P1277_ADMISSION(v!=0,true);
    if(__atomic_load_n(&hakios_p12753_busy,__ATOMIC_ACQUIRE) || __atomic_load_n(&hakios_p125_lease,__ATOMIC_ACQUIRE))return EBUSY;
    if(v && (!p12752_boot_enabled() || (__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE)<0 || (__atomic_load_n(&p12751_latched,__ATOMIC_ACQUIRE)&3)!=3) ||
       __atomic_load_n(&hakios_p1274_deadline,__ATOMIC_ACQUIRE) ||
       p125_thermal_reason() || hakios_battery_temperature()<=0))return EPERM;
    lck_spin_lock(&p12752_lock);
    if(v){
        if(p12752_idle_mode && mach_absolute_time()<p12752_idle_until){lck_spin_unlock(&p12752_lock);return EBUSY;}
        p12752_idle_count=p12752_idle_drops=p12752_idle_reason=p12752_bus_next=0;
        p12752_idle_phase=p12752_idle_last=p12752_bus_last=p12752_idle_busy=p12752_guard_last=0;
        p12752_idle_until=mach_absolute_time()+p1274_ticks_ms(150000);
    }
    p12752_idle_mode=(unsigned)v;
    lck_spin_unlock(&p12752_lock);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_idle_mode,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_idle_mode_sysctl,"I","P12752 quiet window 1 thermal 2 thermal+read-only bus; one P125 phase");
static void p12752_idle_append_locked(const struct p12752_idle_row *row){
    if(p12752_idle_count<P12752_IDLE_ROWS)p12752_idle_rows[p12752_idle_count++]=*row;
    else p12752_idle_drops++;
}
static void p12752_idle_tick(uint64_t now){
    if(!p12752_boot_enabled())return;
    uint64_t phase=__atomic_load_n(&hakios_p125_start,__ATOMIC_ACQUIRE);
    unsigned active=hakios_p125_active(),mode=0,reason=0,index=UINT32_MAX;
    struct p12752_idle_row r={0};
    lck_spin_lock(&p12752_lock);
    mode=p12752_idle_mode;
    if(!mode){lck_spin_unlock(&p12752_lock);return;}
    if(!active || !phase){
        if(p12752_idle_phase || now>=p12752_idle_until){
            if(!p12752_idle_phase)p12752_idle_reason=8;
            p12752_idle_mode=0;
        }
        lck_spin_unlock(&p12752_lock);return;
    }
    if(!p12752_idle_phase)p12752_idle_phase=phase;
    if(phase!=p12752_idle_phase){p12752_idle_mode=0;lck_spin_unlock(&p12752_lock);return;}
    if(now>=p12752_idle_until)reason=8;
    if(clusters[0].sum_now+clusters[1].sum_now>=200u){
        if(!p12752_idle_busy)p12752_idle_busy=now;
        if(now-p12752_idle_busy>=p1274_ticks_ms(500))reason=8;
    }else p12752_idle_busy=0;
    if(!reason && p12752_guard_last && now-p12752_guard_last<p1274_ticks_ms(200)){
        lck_spin_unlock(&p12752_lock);return;
    }
    p12752_guard_last=now;
    lck_spin_unlock(&p12752_lock);
    /* Direct CPU sensor reads only while explicitly measuring, 5Hz max.
     * Do not poll ACPM while holding the spinlock. */
    r.at=now;r.kind=1;r.big=hakios_exynos_tmu_zone_now_mc(0);r.lit=hakios_exynos_tmu_zone_now_mc(1);
    r.gpu=hakios_exynos_tmu_zone_mc(2);r.age=hakios_exynos_tmu_age_ms();r.bat=hakios_battery_temperature();
    r.valid=r.big>=0 && r.lit>=0 && r.gpu>=0 && r.age<=2000 && r.bat>0;
    r.bs=clusters[1].sum_now;r.ls=clusters[0].sum_now;r.bkhz=clusters[1].cur;r.lkhz=clusters[0].cur;
    r.bmeas=clusters[1].measured;r.lmeas=clusters[0].measured;
    r.policy=hakios_p127_mode();r.driver=hakios_p126_active(P126_MASK);
    r.refresh=hakios_scanout_refreshes();
    if(!r.valid)reason=3;
    else if(r.big>=67000 || r.lit>=66000 || r.gpu>=68000 || r.bat>=45)reason=2;
    else if(hakios_display_requested_on()!=0)reason=4;
    lck_spin_lock(&p12752_lock);
    if(p12752_idle_mode!=mode || p12752_idle_phase!=phase || !hakios_p125_active()){
        lck_spin_unlock(&p12752_lock);return;
    }
    if(reason || !p12752_idle_last || now-p12752_idle_last>=p1274_ticks_ms(2000)){
        p12752_idle_append_locked(&r);p12752_idle_last=now;
    }
    if(reason){p12752_idle_reason=reason;p12752_idle_mode=0;}
    else if(mode==2 && r.policy==1 && r.driver==15u && p12752_bus_next<13 &&
       (!p12752_bus_last || now-p12752_bus_last>=p1274_ticks_ms(4000))){
        index=p12752_bus_next++;p12752_bus_last=now;
    }
    lck_spin_unlock(&p12752_lock);
    if(reason){
        /* End the existing observation and experimental leases, not hardware.
         * Preserve old hard stops; early cutoff does not grant qualification. */
        p125_finish(reason);hakios_p127_stop(reason);hakios_p126_stop(reason);return;
    }
    if(index<13){
        struct p12752_idle_row b={0};b.kind=2;b.index=index;
        uint64_t start=mach_absolute_time();b.error=hakios_acpm_dvfs_get(index,&b.raw);
        b.at=mach_absolute_time();b.latency=b.at-start;b.valid=!b.error && b.raw!=0;
        lck_spin_lock(&p12752_lock);
        if(p12752_idle_mode==mode && p12752_idle_phase==phase && hakios_p125_active())p12752_idle_append_locked(&b);
        lck_spin_unlock(&p12752_lock);
    }
}
static int p12752_idle_info_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[160];lck_spin_lock(&p12752_lock);
    snprintf(buf,sizeof(buf),"version=1 mode=%u phase=%llu count=%u drops=%u reason=%u bus=%u",
        p12752_idle_mode,(unsigned long long)p12752_idle_phase,p12752_idle_count,p12752_idle_drops,p12752_idle_reason,p12752_bus_next);
    lck_spin_unlock(&p12752_lock);return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_idle_info,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_idle_info_sysctl,"A","P12752 phase-keyed idle coverage");
static int p12752_idle_select_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    int v=(int)__atomic_load_n(&p12752_idle_select,__ATOMIC_ACQUIRE);
    int e=sysctl_io_number(req,v,sizeof(v),&v,NULL);if(e||!req->newptr)return e;
    if(v<0 || v>=(int)P12752_IDLE_ROWS)return EINVAL;
    __atomic_store_n(&p12752_idle_select,(unsigned)v,__ATOMIC_RELEASE);return 0;
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_idle_select,CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_idle_select_sysctl,"I","P12752 idle row selector");
static int p12752_idle_page_sysctl(__unused struct sysctl_oid *oidp,__unused void *a,__unused int b,struct sysctl_req *req){
    char buf[448];struct p12752_idle_row r;uint64_t phase;
    unsigned s=__atomic_load_n(&p12752_idle_select,__ATOMIC_ACQUIRE);
    lck_spin_lock(&p12752_lock);
    if(hakios_p125_active() || s>=p12752_idle_count){lck_spin_unlock(&p12752_lock);return EBUSY;}
    r=p12752_idle_rows[s];phase=p12752_idle_phase;lck_spin_unlock(&p12752_lock);
    snprintf(buf,sizeof(buf),"version=1 select=%u phase=%llu at=%llu kind=%u index=%u raw=%u valid=%u errno=%d latency=%llu big=%d lit=%d gpu=%d bat=%d age=%u bs=%u ls=%u bkhz=%u lkhz=%u bmeas=%u lmeas=%u policy=%u driver=%u refresh=%u",
        s,(unsigned long long)phase,(unsigned long long)r.at,r.kind,r.index,r.raw,r.valid,r.error,(unsigned long long)r.latency,
        r.big,r.lit,r.gpu,r.bat,r.age,r.bs,r.ls,r.bkhz,r.lkhz,r.bmeas,r.lmeas,r.policy,r.driver,r.refresh);
    return SYSCTL_OUT(req,buf,strlen(buf)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p12752_idle_page,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p12752_idle_page_sysctl,"A","P12752 phase row; read only");
#endif
