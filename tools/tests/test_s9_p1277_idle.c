/* Compile the actual automatic owner against deterministic fake clock,
 * UFS, PMIC, and lease consumers. No real device operations. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#define __unused __attribute__((unused))
#define LCK_GRP_DECLARE(n,s) int n
#define LCK_MTX_DECLARE(n,g) int n
#define SYSCTL_PROC(...)
#define P1277_ADMISSION(enter,legacy) do{if((enter)&&(legacy)&&p1277_auto_blocks())return EBUSY;}while(0)
#define SYSCTL_OUT(req,p,n) (0)
#define P127_MASK 1023u
#define P127_FLOOR(m) (((m)>>4)&7u)
#define P127545_COMPOSITE 69u
#define SCION_BIG_CORES 4
#define LEVELS_MOST 16
#define HAKIOS_ACPM_PMIC_PMIC 0
struct sysctl_oid{int unused;}; struct sysctl_req{void *newptr;};
static int sysctl_io_number(struct sysctl_req*r,int a,size_t n,int*b,void*c){(void)r;(void)a;(void)n;(void)b;(void)c;return 0;}
static void lck_spin_lock(int*p){(void)p;}
static void lck_spin_unlock(int*p){(void)p;}
static bool lck_mtx_try_lock(int*p){(void)p;return true;}
static void lck_mtx_unlock(int*p){(void)p;}
static uint64_t tick=100000, fault_seq;
static uint64_t p1277_auto_lease,hakios_p127_lease,hakios_p125_lease,hakios_p126_lease,hakios_p1274_deadline,p124_cpu_until,hakios_p1273_offline_duration;
static uint32_t p1277_auto_permission=1,p1277_auto_enabled=1,p1277_active_enabled,p1277_active_permission,p1277_g2_permission,p1277_psci_permission;
static uint32_t hakios_p12753_busy,p12_cpu_mode,p8_big_cap_khz,hakios_p1273_offline_fixed,hakios_p1273_trace_state,p1273_cap_state,hakios_p1273_gate_watch;
static uint32_t hakios_p127_reason,hakios_p1273_last_stop_reason,hakios_p1274_trace_late;
static uint64_t hakios_p1273_trace_generation,hakios_p1273_trace_trigger;
static bool hakios_p1273_trace_arm(uint64_t now,uint64_t until,uint64_t span){
    assert(until>now && span);if(hakios_p1273_trace_state)return false;
    hakios_p1273_trace_state=2;hakios_p1273_trace_generation++;hakios_p1273_trace_trigger=0;return true;
}
static void hakios_p1273_trace_stop(unsigned reason,uint64_t now){(void)reason;(void)now;hakios_p1273_trace_state=4;}
static unsigned p125_thermal_reason(void){return 0;}
static uint64_t hakios_p1273_last_stop_lease,hakios_p127_abox[4],touch_last=1;
static bool p124_enabled=true,gov_running=true,ocp_on,ufs_safe=true,ufs_busy=false,quiet=true,display_on=true,panel_owner=false,panel_compatible=false;
static unsigned gov_policy=2,big_cores=4,hakios_cpu_exit_mask,p1273_gate_phase;
static int p1273_gate_lock,p12752_lock,temperature=61000,pmic_error;
static unsigned p12752_idle_mode;
static bool bad_table,wrong_rate;
struct cluster{unsigned online,sum_now,cur,touch_khz,frame_khz,scion_touch,boost_floor;} clusters[2]={{4,0,1053000,1456000,0},{4,0,650000,1800000,1200000}};
#include "../../drivers/power/exynos/hakios.p12781_active.h"
static bool p12781_profile=true;
static uint64_t p12781_tail_hits,p12781_tail_bypass;
#define NSEC_PER_MSEC 1000000ull
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
static uint64_t p1277_boost_span(void){return 150*NSEC_PER_MSEC;}
static uint64_t p1277_burst_span(void){return 60*NSEC_PER_MSEC;}
static bool p1277_active_now(void){return false;}
static unsigned p1277_touch_floor(struct cluster *c,unsigned f){(void)c;return f;}
static uint64_t mach_absolute_time(void){return tick;}
static uint64_t p1274_ticks_ms(unsigned ms){return (uint64_t)ms*1024;}
static bool hakios_p123_input_quiet(uint64_t n){(void)n;return quiet;}
static int hakios_display_requested_on(void){return display_on;}
static bool hakios_p124_ufs_safe(void){return ufs_safe;}
static bool hakios_p124_ufs_busy(void){return ufs_busy;}
static int hakios_exynos_tmu_zone_mc(int z){(void)z;return temperature;}
static int hakios_battery_temperature(void){return 36;}
static unsigned hakios_exynos_tmu_age_ms(void){return 10;}
static bool hakios_p9_panel_auto_on(void){return panel_owner;}
bool hakios_p1277_panel_auto_compatible(void){return panel_compatible;}
static unsigned driver_owner;
static bool driver_qualified=true,driver_fail_begin,driver_fail_end,driver_live=true;
static bool hakios_p545_mfc_restored(void){return driver_owner!=1;}
static bool hakios_p545_cam_restored(void){return driver_owner!=2;}
static bool hakios_p545_disp_restored(void){return driver_owner<3;}
static bool fake_begin(unsigned m,uint64_t until){assert(until>tick && !driver_owner);if(driver_fail_begin)return false;driver_owner=m;return true;}
static bool fake_end(unsigned m){if(driver_fail_end || driver_owner!=m)return false;driver_owner=0;return true;}
static bool hakios_p127545_mfc_qualified(void){return driver_qualified;}
static bool hakios_p127545_cam_qualified(void){return driver_qualified;}
static bool hakios_p545_disp_qualified(unsigned m){return driver_qualified && (m==200000 || m==134000);}
static bool hakios_p127545_mfc_begin(uint64_t u){return fake_begin(1,u);}
static bool hakios_p127545_cam_begin(uint64_t u){return fake_begin(2,u);}
static bool hakios_p545_disp_begin(unsigned m,uint64_t u){return fake_begin(m==200000?3:4,u);}
static bool hakios_p127545_mfc_end(void){return fake_end(1);}
static bool hakios_p127545_cam_end(void){return fake_end(2);}
static bool hakios_p545_disp_end(void){return driver_owner>=3 && fake_end(driver_owner);}
static bool hakios_p127545_mfc_live(void){return driver_owner==1 && driver_live;}
static bool hakios_p127545_cam_live(void){return driver_owner==2 && driver_live;}
static bool hakios_p545_disp_live(void){return driver_owner>=3 && driver_live;}
static bool p12754_optin(void){return true;}
static bool p1273_gate_enabled(void){return true;}
static bool p127542_g2_safe(void){return display_on;}
static bool p1273_gate_idle(void){return hakios_p127_lease==0 && !hakios_p12753_busy;}
static void p1273_gate_refresh_locked(uint64_t n){(void)n;}
static uint64_t p1273_gate_faults(void){return fault_seq;}
static bool p127_floor_safe(unsigned m){return P127_FLOOR(m)>0 && P127_FLOOR(m)<=4 && !ufs_busy && ufs_safe;}
static unsigned hakios_p127_mode(void){return hakios_p127_lease&P127_MASK;}
static bool p1277_auto_blocks(void){return p1277_auto_enabled || p1277_auto_lease;}
static bool hakios_p127_stop_owned(uint64_t v,unsigned reason){
    if(!v || hakios_p127_lease!=v)return false;
    hakios_p127_lease=0;hakios_p1273_last_stop_lease=v;hakios_p127_reason=hakios_p1273_last_stop_reason=reason;
    return true;
}
static int say(const char*f,...){(void)f;return 0;}
static unsigned hakios_acpm_dvfs_table(unsigned i,uint32_t*k,uint32_t*v,unsigned n,int(*log)(const char*,...)){
    (void)i;(void)n;(void)log;
    const uint32_t khz[]={949000,832000,715000,598000},uv[]={731250,700000,668750,637500};
    memcpy(k,khz,sizeof(khz));memcpy(v,uv,sizeof(uv));if(bad_table)v[3]++;return 4;
}
static int hakios_acpm_pmic_read(int i,int addr,uint8_t*v){
    (void)i;(void)addr;
    const uint8_t vals[]={69,64,59,54};
    unsigned f=P127_FLOOR(hakios_p127_mode());
    if(!f || f>4)return 1;
    *v=vals[f-1];return pmic_error;
}
static uint32_t measure_mhz(struct cluster*c){return wrong_rate?0:c->cur/1000;}
static bool p1273_rate_matches(unsigned m,unsigned c){return m*1000==c;}
#include "../../drivers/power/exynos/hakios.p1277_idle.h"
static void advance(unsigned ms){
    tick+=p1274_ticks_ms(ms);
    unsigned mode=hakios_p127_mode(),f=P127_FLOOR(mode);
    static const unsigned rates[]={1053000,949000,832000,715000,598000};
    clusters[0].cur=rates[f];
    p1277_auto_tick(tick);
}
static void reset(void){
    p1277_auto_state=P77_WAIT;p1277_auto_stage=p1277_auto_fault=p1277_auto_samples=p1277_auto_retries=p1277_auto_abox_ok=0;
    p1277_auto_quiet=p1277_auto_started=p1277_auto_sample_at=p1277_auto_restore_until=0;
    p1277_auto_table_ok=p1277_auto_stage_pass=p1277_auto_was_active=false;
    p1277_auto_enabled=1;p1277_auto_lease=hakios_p127_lease=0;
    p1277_l2_request=p1277_l2_module=p1277_l2_state=p1277_l2_result=p1277_l2_samples=0;
    driver_owner=0;driver_qualified=driver_live=true;driver_fail_begin=driver_fail_end=false;
    panel_owner=panel_compatible=false;
    p1277_burst_request=p1277_burst_owned=0;
    p1277_burst_since=p1277_burst_trigger=p1277_burst_until=0;
    hakios_p1273_trace_state=hakios_p1274_trace_late=0;clusters[0].sum_now=clusters[1].sum_now=0;
    memset(hakios_p127_abox,0,sizeof(hakios_p127_abox));
    display_on=false;ufs_safe=true;ufs_busy=false;quiet=true;bad_table=false;wrong_rate=false;pmic_error=0;fault_seq=0;
}
static void start_trial(void){advance(1);advance(3000);assert(p1277_auto_state==P77_TRIAL);}
int main(void){
    reset();display_on=true;advance(6000);assert(!hakios_p127_lease);
    display_on=false;ufs_safe=false;advance(6000);assert(!hakios_p127_lease);
    ufs_safe=true;ufs_busy=true;advance(6000);assert(!hakios_p127_lease);
    ufs_busy=false;
    for(unsigned stage=0;stage<4;stage++){
        start_trial();assert(p1277_auto_stage==stage);
        advance(3000);assert(p1277_auto_samples==1);
        advance(3000);assert(!hakios_p127_lease);
        advance(1);assert(p1277_auto_state==P77_RESTORE);
        advance(2000);assert(p1277_auto_stage==stage+1 && !p1277_auto_lease);
    }
    start_trial();hakios_p127_abox[0]++;hakios_p127_abox[2]++;
    advance(3000);assert(!hakios_p127_lease);hakios_p127_abox[1]++;
    advance(1);advance(2000);assert(p1277_auto_state==P77_READY && p1277_auto_abox_ok);
    advance(1);advance(3000);assert(p1277_auto_state==P77_ACTIVE && hakios_p127_mode()==69);
    uint64_t old=hakios_p127_lease;
    advance(95000);assert(hakios_p127_lease!=old && hakios_p127_mode()==69);
    display_on=true;advance(1);assert(!hakios_p127_lease);advance(1);advance(2000);assert(!p1277_auto_lease);
    reset();p12752_idle_mode=1;advance(1);advance(3000);assert(!hakios_p127_lease);p12752_idle_mode=0;
    reset();start_trial();ufs_busy=true;advance(1);assert(!hakios_p127_lease);ufs_busy=false;advance(1);advance(2000);assert(!p1277_auto_lease && p1277_auto_stage==0);
    reset();start_trial();uint64_t stale=p1277_auto_lease;hakios_p127_lease=0;hakios_p1273_last_stop_lease=stale-1024;advance(1);advance(2000);assert(p1277_auto_lease==stale);advance(16000);assert(p1277_auto_fault==4 && p1277_auto_lease==stale && !p1277_auto_enabled);
    reset();bad_table=true;advance(1);advance(3000);assert(p1277_auto_fault && !hakios_p127_lease);
    reset();start_trial();pmic_error=1;advance(3000);assert(p1277_auto_fault && !hakios_p127_lease);
    reset();start_trial();advance(3000);display_on=true;advance(1);advance(1);advance(2000);assert(p1277_auto_stage==0 && !p1277_auto_lease);
    reset();p1277_auto_stage=5;advance(1);advance(3000);assert(p1277_auto_state==P77_ACTIVE);
    p1277_l2_request=1;advance(1);assert(driver_owner==1 && p1277_l2_state==P77_L2_HELD);
    for(unsigned i=0;i<20;i++)advance(1000);
    assert(!driver_owner && p1277_l2_result==1 && !p1277_l2_state);
    driver_qualified=false;p1277_l2_request=2;advance(1);assert(!driver_owner && p1277_l2_result==3);
    driver_qualified=true;panel_owner=panel_compatible=true;p1277_l2_request=3;advance(1);assert(!driver_owner && p1277_l2_result==3);
    panel_owner=false;p1277_l2_request=4;advance(1);assert(driver_owner==4);
    display_on=true;advance(1);assert(!driver_owner && p1277_l2_result==2 && !hakios_p127_lease);
    reset();p1277_auto_stage=5;advance(1);advance(3000);
    p1277_l2_request=2;advance(1);assert(driver_owner==2);
    driver_fail_end=true;display_on=true;advance(1);assert(!hakios_p127_lease && p1277_auto_lease);
    advance(6000);assert(p1277_l2_result==4 && p1277_auto_fault==6 && p1277_auto_state==P77_FAULT && p1277_auto_lease);
    reset();p1277_auto_stage=5;advance(1);advance(3000);assert(hakios_p127_lease);
    p1277_burst_request=1;p1277_burst_until=tick+p1274_ticks_ms(120000);
    clusters[0].sum_now=150;advance(1);advance(250);
    assert(!hakios_p127_lease && p1277_auto_lease && !hakios_p1273_trace_state);
    advance(2000);assert(!p1277_auto_lease);
    advance(1);assert(p1277_burst_owned && !p1277_burst_request && hakios_p1273_trace_state==2 && !hakios_p127_lease);
    p1277_burst_cancel(tick);assert(hakios_p1273_trace_state==4);
    reset();p1277_burst_request=1;p1277_burst_until=tick+p1274_ticks_ms(10);
    advance(20);assert(!p1277_burst_request && !hakios_p1273_trace_state);
    puts("P1277 burst: sustained trigger, exact restore before trace, cancellation and timeout PASS (mock)");
    puts("P1277 layer2: clean, unqualified, panel conflict, display preemption and uncertain rollback PASS (mock)");
    puts("P1277 actual idle header: delayed admission, staged VSEL, ABOX, renewal, wake, refusal PASS (mock)");
}
