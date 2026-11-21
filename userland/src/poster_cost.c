/*
 * S9 Poster render-cost proxy, without changing wallpapers or services.
 * Called from rc.jobs, but completely exits unless boot-arg poster_probe=1.
 * Captures per-process CPU time, resident size, VM, faults, page-ins,
 * context switches and system calls every N seconds, including processes
 * outside cputop's top-ten. These are PROCESS costs, not GPU render time.
 *
 * Compare the same timestamps to fps:/cpufreq_gov: and HakiosMetal's own
 * per-process command-buffer telemetry for the graphics-specific picture.
 */
#include <mach/mach_time.h>
#include <errno.h>
#include "s9_p4_thread_delta.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PROC_ALL_PIDS 1
#define PROC_PIDTASKINFO 4
#define MAX_PROCS 2048
#define MAX_MATCHES 64
struct proc_taskinfo {
    uint64_t pti_virtual_size,pti_resident_size,pti_total_user,pti_total_system;
    uint64_t pti_threads_user,pti_threads_system;
    int32_t pti_policy,pti_faults,pti_pageins,pti_cow_faults;
    int32_t pti_messages_sent,pti_messages_received;
    int32_t pti_syscalls_mach,pti_syscalls_unix,pti_csw;
    int32_t pti_threadnum,pti_numrunning,pti_priority;
};
int proc_listpids(uint32_t, uint32_t, void *, int);
int proc_pidinfo(int, int, uint64_t, void *, int);
int proc_name(int, void *, uint32_t);

struct snap {int pid;char name[64];struct proc_taskinfo info;};
static const char *targets[]={"backboardd","CollectionsPoster","PosterBoard","SpringBoard","chronod","poster_cost","PridePoster","PhotosPosterProvider","GradientPosterExtension","EmojiPosterExtension","usbmuxd","hapticsd","powerlogd","progressd","wifid"};
#define P6_TARGET_COUNT (sizeof(targets)/sizeof(targets[0]))
static int p6_last_pid[P6_TARGET_COUNT];
static uint32_t p6_pid_changes[P6_TARGET_COUNT],p6_missed[P6_TARGET_COUNT];
/* P6.2 fix: same-name processes can coexist. Previous code compared one
 * singleton PID per NAME and falsely reported hundreds of restarts.
 * Use bounded PID sets across complete census iterations instead. */
#define P6_MAX_IDENTITIES 8
static int p6_pid_prev[P6_TARGET_COUNT][P6_MAX_IDENTITIES];
static int p6_pid_cur[P6_TARGET_COUNT][P6_MAX_IDENTITIES];
static unsigned p6_pid_prev_n[P6_TARGET_COUNT],p6_pid_cur_n[P6_TARGET_COUNT];
/* P6.1 source-specific counters from matching S9 kernel drivers, not
 * actual CPU wakeup-residency events. Missing sysctl stays visibly invalid. */
static const char *const p6_source_nodes[]={
    "kern.hakios.p6_usb_ticks","kern.hakios.p6_usb_connected_ticks",
    "kern.hakios.p6_usb_overlap_ticks","kern.hakios.p6_touch_irqs",
    "kern.hakios.p6_touch_polls","kern.hakios.p6_wifi_irqs"
};
static uint64_t p6_source_prev[6];
static uint32_t p6_source_prev_valid;
/* P6.9 source-specific periodic callback/IRQ pin/key counts; 1s snapshots
 * only when p69_mode=1. First sample is intentionally invalid. */
static const char *const p69_source_nodes[]={
    "kern.hakios.p69_usb_disconnected_ticks",
    "kern.hakios.p69_usb_link_stopped_ticks",
    "kern.hakios.p69_touch_pin_low",
    "kern.hakios.p69_touch_event_reads",
    "kern.hakios.p69_key_polls",
    "kern.hakios.p69_key_power_down",
    "kern.hakios.p69_key_power_up",
    "kern.hakios.p69_key_held_polls"
};
static uint64_t p69_source_prev[8],p69_prev_up;
static uint32_t p69_source_prev_valid;
static int p69_prev_phase=-1;
static struct snap prev[MAX_MATCHES],now[MAX_MATCHES];
static int prev_count;

/* P4 per-thread passive attribution: best-effort libproc information from
 * the existing poster_cost process. No task_for_pid, suspend, signals,
 * priority writes or sampling profiler. When disabled: ZERO thread calls.
 *
 * PROC_PIDLISTTHREADS/PIDTHREADINFO are the libproc ABI on this iOS port.
 * The kernel's time unit for pth_user_time may be mach ticks or ns; raw
 * deltas and both CPU-percent interpretations are logged for calibration.
 */
#define PROC_PIDTHREADINFO 5
#define PROC_PIDLISTTHREADS 6
#define P4_MAX_TIDS 384
#define P4_MAX_LAST 1152
#define P4_TOP 6
struct p4_threadinfo {
    uint64_t user,system;
    int32_t cpu_usage,policy,run_state,flags,sleep_time,curpri,priority,maxpriority;
    char name[64];
};
struct p4_thread_last {int pid;uint64_t tid,user,system;};
struct p4_thread_hot {
    uint64_t tid,du,ds;
    int32_t run_state,sleep_hint,flags,policy,curpri,priority;
    char name[64];
};
static struct p4_thread_last p4_before[P4_MAX_LAST],p4_after[P4_MAX_LAST];
static unsigned p4_n_before,p4_n_after;
static uint64_t p4_thread_ids[P4_MAX_TIDS];
static struct p4_thread_hot p4_hot[P4_MAX_TIDS];

static const struct p4_thread_last *
p4_prev_thread(int pid,uint64_t tid)
{
    for(unsigned i=0;i<p4_n_before;i++)
        if(p4_before[i].pid==pid && p4_before[i].tid==tid)return &p4_before[i];
    return NULL;
}
static int
p4_hot_compare(const void *x,const void *y)
{
    const struct p4_thread_hot *a=x,*b=y;
    uint64_t va=a->du+a->ds,vb=b->du+b->ds;
    return va>vb?-1:va<vb?1:0;
}
static int
p4_sysctl_int(const char *name,int fallback)
{
    int value=fallback;
    size_t size=sizeof(value);
    if(sysctlbyname(name,&value,&size,NULL,0)!=0 || size!=sizeof(value))
        return fallback;
    return value;
}
static int
p4_thread_target(int setting,const char *name)
{
    return !strcmp(name,"backboardd") ||
        (setting>=2 && (!strcmp(name,"SpringBoard") || !strcmp(name,"PosterBoard")));
}
static void
p4_sample_threads(int pid,const char *procname,unsigned setting,int phase,
    uint64_t uptime,uint64_t elapsed_ticks,double mach_to_ns,int warmup,int p5_mode,int p6_ca,int p6_wake)
{
    const int length=proc_pidinfo(pid,PROC_PIDLISTTHREADS,0,p4_thread_ids,sizeof(p4_thread_ids));
    if(length<=0){
        printf("p4-thread: up=%llus phase=%d name=%s pid=%d unavailable=thread-list errno=%d (no process disruption)\n",
            (unsigned long long)uptime,phase,procname,pid,errno);
        return;
    }
    unsigned count=(unsigned)length/sizeof(p4_thread_ids[0]);
    if(count>P4_MAX_TIDS)count=P4_MAX_TIDS;
    unsigned read=0,hot=0,missing=0,active_states=0,asleep_states=0;
    for(unsigned k=0;k<count;k++){
        struct p4_threadinfo ti;
        int got=proc_pidinfo(pid,PROC_PIDTHREADINFO,p4_thread_ids[k],&ti,sizeof(ti));
        if(got!=(int)sizeof(ti)){missing++;continue;}
        read++;
        /* run_state is a point-in-time libproc snapshot, NOT a count
         * of scheduler wakeups, core residency or thread migrations. */
        active_states += ti.run_state == 3;
        asleep_states += ti.run_state != 3;
        if(p4_n_after<P4_MAX_LAST){
            struct p4_thread_last *cur=&p4_after[p4_n_after++];
            cur->pid=pid;cur->tid=p4_thread_ids[k];
            cur->user=ti.user;cur->system=ti.system;
        }
        const struct p4_thread_last *old=p4_prev_thread(pid,p4_thread_ids[k]);
        if(warmup || !old)continue;
        uint64_t du=s9_p4_delta(old->user,ti.user),ds=s9_p4_delta(old->system,ti.system);
        char thread_name[sizeof(ti.name)+1];
        memcpy(thread_name,ti.name,sizeof(ti.name));
        thread_name[sizeof(ti.name)]=0; /* libproc may return a full non-NUL array */
        if((p5_mode || p6_ca) && !strcmp(procname,"backboardd") &&
            strstr(thread_name,"coreanimation.display.primary")) {
            double ns=(double)elapsed_ticks*mach_to_ns;
            printf("p5-display-thread: up=%llus phase=%d tid=%llu u_ns=%llu s_ns=%llu cpu_if_ns=%.2f run=%d policy=%d curpri=%d sleep_hint=%d source=libproc_5s_or_1s_not_wakeup_trace\n",
                (unsigned long long)uptime,phase,
                (unsigned long long)p4_thread_ids[k],
                (unsigned long long)du,(unsigned long long)ds,
                ns>0?100.0*(double)(du+ds)/ns:0.0,ti.run_state,
                ti.policy,ti.curpri,ti.sleep_time);
            if(p6_ca)
                printf("p6-ca-thread: up=%llus phase=%d pid=%d tid=%llu cpu_ns=%.2f run=%d sleep_hint=%d policy=%d priority=%d source=sampled_libproc_no_stack_or_wakeup_origin\n",
                    (unsigned long long)uptime,phase,pid,
                    (unsigned long long)p4_thread_ids[k],
                    ns>0?100.0*(double)(du+ds)/ns:0.0,
                    ti.run_state,ti.sleep_time,ti.policy,ti.curpri);
        }
        if((du || ds) && hot<P4_MAX_TIDS){
            struct p4_thread_hot *h=&p4_hot[hot++];
            h->tid=p4_thread_ids[k];h->du=du;h->ds=ds;
            h->run_state=ti.run_state;h->sleep_hint=ti.sleep_time;h->flags=ti.flags;
            h->policy=ti.policy;h->curpri=ti.curpri;h->priority=ti.priority;
            memcpy(h->name,ti.name,sizeof(h->name));
            h->name[sizeof(h->name)-1]=0;
            for(unsigned c=0;c<sizeof(h->name) && h->name[c];c++)
                if(h->name[c]==' ' || h->name[c]=='\t' || h->name[c]=='\n')
                    h->name[c]='_';
        }
    }
    qsort(p4_hot,hot,sizeof(p4_hot[0]),p4_hot_compare);
    if(p6_wake)
        printf("p6-thread-snapshot: up=%llus phase=%d name=%s pid=%d listed=%u read=%u active_state3=%u other_states=%u missing=%u source=sampled_states_not_wakeups\n",
            (unsigned long long)uptime,phase,procname,pid,count,read,
            active_states,asleep_states,missing);
    if(p5_mode)
        printf("p5-thread-state: up=%llus phase=%d name=%s pid=%d listed=%u read=%u active_state3=%u other_states=%u missing=%u source=sampled_run_state_not_wakeup_count\n",
            (unsigned long long)uptime,phase,procname,pid,count,read,
            active_states,asleep_states,missing);
    printf("p4-threads: up=%llus phase=%d name=%s pid=%d listed=%u read=%u missing=%u hot=%u truncated=%u unit=uncalibrated\n",
        (unsigned long long)uptime,phase,procname,pid,count,read,missing,hot,
        (unsigned)(length==(int)sizeof(p4_thread_ids)));
    if(warmup || !elapsed_ticks) return;
    double elapsed_ns=(double)elapsed_ticks*mach_to_ns;
    for(unsigned k=0;k<hot && k<P4_TOP;k++){
        const struct p4_thread_hot *h=&p4_hot[k];
        double cpu_mach=100.0*(double)(h->du+h->ds)/(double)elapsed_ticks;
        double cpu_ns=elapsed_ns>0?100.0*(double)(h->du+h->ds)/elapsed_ns:0.0;
        printf("p4-thread: up=%llus phase=%d name=%s pid=%d tid=%llu thr=%s u_raw=%llu s_raw=%llu cpu_if_mach=%.2f cpu_if_ns=%.2f run=%d sleep_hint=%d flags=%#x\n",
            (unsigned long long)uptime,phase,procname,pid,
            (unsigned long long)h->tid,h->name[0]?h->name:"(unnamed)",
            (unsigned long long)h->du,(unsigned long long)h->ds,
            cpu_mach,cpu_ns,h->run_state,h->sleep_hint,(unsigned)h->flags);
        if(p5_mode)
            printf("p5-thread: up=%llus phase=%d proc=%s pid=%d tid=%llu thr=%s u_ns=%llu s_ns=%llu cpu_if_ns=%.2f policy=%d curpri=%d priority=%d run=%d sleep_hint=%d flags=%#x source=sampled_libproc_not_core_trace\n",
                (unsigned long long)uptime,phase,procname,pid,
                (unsigned long long)h->tid,h->name[0]?h->name:"(unnamed)",
                (unsigned long long)h->du,(unsigned long long)h->ds,cpu_ns,
                h->policy,h->curpri,h->priority,h->run_state,
                h->sleep_hint,(unsigned)h->flags);
    }
}

static void
p6_sample_driver_sources(int phase,uint64_t uptime)
{
    uint64_t delta[6]={0};
    uint32_t mask=0,baseline=0;
    for(unsigned i=0;i<6;i++){
        uint64_t now=0;
        size_t size=sizeof(now);
        if(sysctlbyname(p6_source_nodes[i],&now,&size,NULL,0)!=0 || size!=sizeof(now)){
            p6_source_prev_valid &= ~(1u<<i);
            continue;
        }
        mask|=1u<<i;
        if((p6_source_prev_valid&(1u<<i)) && now>=p6_source_prev[i]){
            delta[i]=now-p6_source_prev[i];
            baseline|=1u<<i;
        }
        p6_source_prev[i]=now;
        p6_source_prev_valid|=1u<<i;
    }
    printf("p6-source: up=%llus phase=%d usb_ticks=%llu usb_connected=%llu usb_overlap=%llu touch_irq=%llu touch_poll=%llu wifi_irq=%llu present_mask=%#x delta_valid=%#x source=USB_thread_call_touch_GIC_handler_or_poll_not_all_CPU_wakeups\n",
        (unsigned long long)uptime,phase,
        (unsigned long long)delta[0],(unsigned long long)delta[1],
        (unsigned long long)delta[2],(unsigned long long)delta[3],
        (unsigned long long)delta[4],(unsigned long long)delta[5],mask,baseline);
}
static void
p69_sample_driver_sources(int phase,uint64_t uptime)
{
    uint64_t delta[8]={0};
    uint32_t present=0,valid=0;
    uint64_t span=(p69_prev_up && uptime>=p69_prev_up)?
        uptime-p69_prev_up:0;
    /* Interrupted/moved phases must restart the counter timebase. */
    if(p69_prev_phase!=phase || span==0 || span>3)
        p69_source_prev_valid=0;
    for(unsigned i=0;i<8;i++){
        uint64_t now=0;
        size_t n=sizeof(now);
        if(sysctlbyname(p69_source_nodes[i],&now,&n,NULL,0)!=0 || n!=sizeof(now)){
            p69_source_prev_valid&=~(1u<<i);
            continue;
        }
        present|=1u<<i;
        if((p69_source_prev_valid&(1u<<i)) && now>=p69_source_prev[i]){
            delta[i]=now-p69_source_prev[i];
            valid|=1u<<i;
        }
        p69_source_prev[i]=now;
        p69_source_prev_valid|=1u<<i;
    }
    p69_prev_up=uptime;p69_prev_phase=phase;
    printf("p69-source: up=%llus phase=%d span_s=%llu usb_disconnected=%llu usb_stopped=%llu touch_low=%llu touch_reads=%llu key_polls=%llu key_down=%llu key_up=%llu key_held=%llu present_mask=%#x delta_valid=%#x source=callback_GPIO_poll_transition_not_CPU_idle_exit\n",
        (unsigned long long)uptime,phase,(unsigned long long)span,
        (unsigned long long)delta[0],(unsigned long long)delta[1],
        (unsigned long long)delta[2],(unsigned long long)delta[3],
        (unsigned long long)delta[4],(unsigned long long)delta[5],
        (unsigned long long)delta[6],(unsigned long long)delta[7],present,valid);
}
static int selected(const char *name) {
    /* Darwin proc_name may truncate an extension's long executable name.
     * Treat PridePosterExtension / PridePosterExten as ONE census identity;
     * a PID transition still is not itself evidence of SIGSEGV. */
    if(!strncmp(name,"PridePoster",11))return 6;
    for(size_t i=0;i<sizeof(targets)/sizeof(targets[0]);i++)
        if(!strcmp(name,targets[i]))return (int)i;
    return -1;
}
static int boot_enabled(void) {
    char args[4096]={0};size_t n=sizeof(args)-1;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0))return 0;
    args[n<sizeof(args)?n:sizeof(args)-1]=0;
    const char *key="poster_probe=1";
    for(char *p=strstr(args,key);p;p=strstr(p+1,key))
        if((p==args || p[-1]==' ' || p[-1]=='\t') &&
           (!p[strlen(key)] || p[strlen(key)]==' ' || p[strlen(key)]=='\t'))
            return 1;
    return 0;
}
static const struct snap *previous(int pid,const char *name) {
    for(int i=0;i<prev_count;i++)
        if(prev[i].pid==pid && !strcmp(prev[i].name,name))return &prev[i];
    return NULL;
}
int main(int argc,char **argv) {
    if(!boot_enabled())return 0;
    unsigned interval=argc>1?(unsigned)atoi(argv[1]):5;
    unsigned looks=argc>2?(unsigned)atoi(argv[2]):120;
    if(!interval || interval>60)interval=5;
    if(!looks || looks>3600)looks=120;
    int pids[MAX_PROCS];
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    uint64_t beginning=mach_absolute_time(),previous_at=beginning;
    setvbuf(stdout,NULL,_IOLBF,0);
    printf("poster_cost: enabled; %us intervals, %u samples; CPU is process time, not GPU time\n",interval,looks);
    /* Fixed 480 iteration budget used to expire early when P6 switched 5s
     * sampling to 1s. poster_probe=1 is an explicit lab bootarg: retain
     * the observer for >=4h wall time, independent of sampling cadence.
     * No additional thread or CPU tuning; dormant mode uses 5s sleeps. */
    const uint64_t budget_s=(uint64_t)interval*looks>14400ull?
        (uint64_t)interval*looks:14400ull;
    printf("poster_cost: lab wall-time budget=%llus (fixed-sample exhaustion disabled)\n",
        (unsigned long long)budget_s);
    for(unsigned look=0;;look++) {
        const int bytes=proc_listpids(PROC_ALL_PIDS,0,pids,sizeof(pids));
        const uint64_t at=mach_absolute_time();
        const double elapsed=(double)(at-previous_at);
        const unsigned long long t=(unsigned long long)((at-beginning)*
            (double)base.numer/base.denom/1e9);
        const unsigned long long uptime=(unsigned long long)(at*
            (double)base.numer/base.denom/1e9);
        int count=0;unsigned mask=0;
        memset(p6_pid_cur_n,0,sizeof(p6_pid_cur_n));
        int p4_mode=p4_sysctl_int("kern.hakios.p4_mode",0);
        int p4_threads=p4_mode>0?p4_sysctl_int("kern.hakios.p4_threads",0):0;
        int p4_phase=p4_mode>0?p4_sysctl_int("kern.hakios.p4_phase",0):0;
        int p5_mode=p4_sysctl_int("kern.hakios.p5_mode",0);
        int p5_threads=p5_mode>0?p4_sysctl_int("kern.hakios.p5_threads",0):0;
        int p5_phase=p5_mode>0?p4_sysctl_int("kern.hakios.p5_phase",0):0;
        int p6_mode=p4_sysctl_int("kern.hakios.p6_mode",0);
        int p6_flags=p6_mode?p4_sysctl_int("kern.hakios.p6_flags",0):0;
        int p6_phase=p6_mode?p4_sysctl_int("kern.hakios.p6_phase",0):0;
        int p6_ca=p6_mode>0 && (p6_flags&16)!=0;
        int p6_wake=p6_mode>=2 && (p6_flags&1)!=0;
        int p69_mode=p4_sysctl_int("kern.hakios.p69_mode",0);
        int selected_threads=p4_threads>p5_threads?p4_threads:p5_threads;
        if((p6_ca || p6_wake) && selected_threads<1)selected_threads=1;
        p4_n_after=0;
        if(selected_threads==0)p4_n_before=0;
        for(int k=0;k<(bytes>0?bytes/(int)sizeof(int):0) && count<MAX_MATCHES;k++) {
            if(pids[k]<=0)continue;
            char name[64]={0};
            if(proc_name(pids[k],name,sizeof(name))<=0 || selected(name)<0)continue;
            struct proc_taskinfo info;
            if(proc_pidinfo(pids[k],PROC_PIDTASKINFO,0,&info,sizeof(info))!=(int)sizeof(info))
                continue;
            const struct snap *old=previous(pids[k],name);
            struct snap *current=&now[count++];
            current->pid=pids[k];memcpy(current->name,name,sizeof(name));current->info=info;
            int target_idx=selected(name);
            mask|=1u<<(unsigned)target_idx;
            if(p6_mode && (p6_flags&2)) {
                unsigned n=p6_pid_cur_n[target_idx];
                if(n<P6_MAX_IDENTITIES){
                    p6_pid_cur[target_idx][n]=pids[k];
                    p6_pid_cur_n[target_idx]=n+1;
                } else if(n==P6_MAX_IDENTITIES){
                    printf("p6-identity-overflow: up=%llus phase=%d name=%s max=%u churn=unmeasurable\n",
                        uptime,p6_phase,name,(unsigned)P6_MAX_IDENTITIES);
                    p6_pid_cur_n[target_idx]=n+1; /* flagged, no churn inference */
                }
                p6_last_pid[target_idx]=pids[k];
                p6_missed[target_idx]=0;
            }
            if(selected_threads>0 && (p4_thread_target(selected_threads,name) ||
                (p6_wake && (!strcmp(name,"usbmuxd") || !strcmp(name,"hapticsd") ||
                    !strcmp(name,"chronod") || !strncmp(name,"PridePoster",11))))) {
                p4_sample_threads(pids[k],name,(unsigned)selected_threads,
                    p6_mode?p6_phase:(p5_mode>0?p5_phase:p4_phase),uptime,
                    at>=previous_at?at-previous_at:0,
                    (double)base.numer/(double)base.denom,
                    !look || p4_n_before==0,p5_mode>0 && p5_threads>0,p6_ca,p6_wake);
            }
            if(!look)continue;
            const uint64_t used=info.pti_total_user+info.pti_total_system;
            const uint64_t prior=old?old->info.pti_total_user+old->info.pti_total_system:0;
            const double cpu=(old && used>=prior && elapsed>0)?100.0*(double)(used-prior)/elapsed:0;
            const int32_t faults=old?info.pti_faults-old->info.pti_faults:0;
            const int32_t pageins=old?info.pti_pageins-old->info.pti_pageins:0;
            const int32_t csw=old?info.pti_csw-old->info.pti_csw:0;
            const int32_t mach=old?info.pti_syscalls_mach-old->info.pti_syscalls_mach:0;
            const int32_t unix=old?info.pti_syscalls_unix-old->info.pti_syscalls_unix:0;
            printf("poster_cost: %llu s: %s[%d] cpu=%.1f%% rss=%lluKiB vm=%lluMiB faults=%d pageins=%d csw=%d mach=%d unix=%d up=%llus %s\n",
                t,name,pids[k],cpu,(unsigned long long)(info.pti_resident_size>>10),
                (unsigned long long)(info.pti_virtual_size>>20),
                faults<0?0:faults,pageins<0?0:pageins,csw<0?0:csw,mach<0?0:mach,unix<0?0:unix,
                uptime,old?"sample":"new-pid");
            if(p6_mode && (p6_flags&2))
                printf("p6-proc: up=%llus phase=%d name=%s pid=%d cpu_pct=%.2f csw=%d faults=%d pageins=%d syscalls=%d valid_delta=%u source=process_counters_not_IRQ\n",
                    uptime,p6_phase,name,pids[k],cpu,csw<0?0:csw,faults<0?0:faults,
                    pageins<0?0:pageins,(mach>0?mach:0)+(unix>0?unix:0),old?1u:0u);
        }
        if(p6_mode && (p6_flags&2)) {
            for(size_t i=0;i<P6_TARGET_COUNT;i++){
                unsigned old_n=p6_pid_prev_n[i],new_n=p6_pid_cur_n[i];
                if(old_n>P6_MAX_IDENTITIES || new_n>P6_MAX_IDENTITIES){
                    p6_pid_prev_n[i]=new_n;
                    if(new_n<=P6_MAX_IDENTITIES)
                        memcpy(p6_pid_prev[i],p6_pid_cur[i],new_n*sizeof(int));
                    continue;
                }
                /* Report true set replacements only, never alternating
                 * enumeration among concurrently live same-name PIDs. */
                for(unsigned j=0;j<old_n;j++){
                    int lost=p6_pid_prev[i][j],still=0;
                    for(unsigned k=0;k<new_n;k++)
                        if(p6_pid_cur[i][k]==lost)still=1;
                    if(still)continue;
                    for(unsigned k=0;k<new_n;k++){
                        int added=p6_pid_cur[i][k],existed=0;
                        for(unsigned m=0;m<old_n;m++)
                            if(p6_pid_prev[i][m]==added)existed=1;
                        if(!existed){
                            p6_pid_changes[i]++;
                            printf("p6-churn: up=%llus phase=%d name=%s prev_pid=%d new_pid=%d changes=%u missing_samples=%u crash=unverified set_checked=1\n",
                                uptime,p6_phase,targets[i],lost,added,
                                p6_pid_changes[i],p6_missed[i]);
                            break;
                        }
                    }
                }
                p6_pid_prev_n[i]=new_n;
                memcpy(p6_pid_prev[i],p6_pid_cur[i],new_n*sizeof(int));
            }
        } else {
            memset(p6_pid_prev_n,0,sizeof(p6_pid_prev_n));
        }
        if(look)for(size_t i=0;i<sizeof(targets)/sizeof(targets[0]);i++)
            if(!(mask&(1u<<i))) {
                if(i<6)printf("poster_cost: %llu s: %s missing\n",t,targets[i]);
                if(p6_mode && (p6_flags&2) && p6_last_pid[i] && ++p6_missed[i]==1)
                    printf("p6-missing: up=%llus phase=%d name=%s last_pid=%d missing=1 crash=unverified\n",
                        uptime,p6_phase,targets[i],p6_last_pid[i]);
            }
        if(p6_mode && (p6_flags&1))
            p6_sample_driver_sources(p6_phase,uptime);
        if(p69_mode>0)
            p69_sample_driver_sources(p6_phase,uptime);
        else {p69_source_prev_valid=0;p69_prev_phase=-1;p69_prev_up=0;}
        if(selected_threads>0) {
            memcpy(p4_before,p4_after,(size_t)p4_n_after*sizeof(p4_before[0]));
            p4_n_before=p4_n_after;
        }
        memcpy(prev,now,(size_t)count*sizeof(now[0]));
        prev_count=count;previous_at=at;
        if(t>=budget_s)break;
        /* P5 burst mode can resolve 1–3s screen-on spikes, at opt-in
         * 1s sampling overhead; mode0/1 preserves the 5s P4 baseline. */
        sleep(((p5_mode>=2 && p5_threads>0) || (p6_mode>=2 && (p6_flags&17)) || p69_mode>0)?1:interval);
    }
    return 0;
}
