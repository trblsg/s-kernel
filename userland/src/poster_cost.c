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
static const char *targets[]={"backboardd","CollectionsPoster","PosterBoard","SpringBoard","chronod","poster_cost"};
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
    int32_t run_state,sleep_hint,flags;
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
    uint64_t uptime,uint64_t elapsed_ticks,double mach_to_ns,int warmup)
{
    const int length=proc_pidinfo(pid,PROC_PIDLISTTHREADS,0,p4_thread_ids,sizeof(p4_thread_ids));
    if(length<=0){
        printf("p4-thread: up=%llus phase=%d name=%s pid=%d unavailable=thread-list errno=%d (no process disruption)\n",
            (unsigned long long)uptime,phase,procname,pid,errno);
        return;
    }
    unsigned count=(unsigned)length/sizeof(p4_thread_ids[0]);
    if(count>P4_MAX_TIDS)count=P4_MAX_TIDS;
    unsigned read=0,hot=0,missing=0;
    for(unsigned k=0;k<count;k++){
        struct p4_threadinfo ti;
        int got=proc_pidinfo(pid,PROC_PIDTHREADINFO,p4_thread_ids[k],&ti,sizeof(ti));
        if(got!=(int)sizeof(ti)){missing++;continue;}
        read++;
        if(p4_n_after<P4_MAX_LAST){
            struct p4_thread_last *cur=&p4_after[p4_n_after++];
            cur->pid=pid;cur->tid=p4_thread_ids[k];
            cur->user=ti.user;cur->system=ti.system;
        }
        const struct p4_thread_last *old=p4_prev_thread(pid,p4_thread_ids[k]);
        if(warmup || !old)continue;
        uint64_t du=s9_p4_delta(old->user,ti.user),ds=s9_p4_delta(old->system,ti.system);
        if((du || ds) && hot<P4_MAX_TIDS){
            struct p4_thread_hot *h=&p4_hot[hot++];
            h->tid=p4_thread_ids[k];h->du=du;h->ds=ds;
            h->run_state=ti.run_state;h->sleep_hint=ti.sleep_time;h->flags=ti.flags;
            memcpy(h->name,ti.name,sizeof(h->name));
            h->name[sizeof(h->name)-1]=0;
            for(unsigned c=0;c<sizeof(h->name) && h->name[c];c++)
                if(h->name[c]==' ' || h->name[c]=='\t' || h->name[c]=='\n')
                    h->name[c]='_';
        }
    }
    qsort(p4_hot,hot,sizeof(p4_hot[0]),p4_hot_compare);
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
    }
}

static int selected(const char *name) {
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
    for(unsigned look=0;look<=looks;look++) {
        const int bytes=proc_listpids(PROC_ALL_PIDS,0,pids,sizeof(pids));
        const uint64_t at=mach_absolute_time();
        const double elapsed=(double)(at-previous_at);
        const unsigned long long t=(unsigned long long)((at-beginning)*
            (double)base.numer/base.denom/1e9);
        const unsigned long long uptime=(unsigned long long)(at*
            (double)base.numer/base.denom/1e9);
        int count=0;unsigned mask=0;
        int p4_mode=p4_sysctl_int("kern.hakios.p4_mode",0);
        int p4_threads=p4_mode>0?p4_sysctl_int("kern.hakios.p4_threads",0):0;
        int p4_phase=p4_mode>0?p4_sysctl_int("kern.hakios.p4_phase",0):0;
        p4_n_after=0;
        if(p4_threads==0)p4_n_before=0;
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
            mask|=1u<<(unsigned)selected(name);
            if(p4_threads>0 && p4_thread_target(p4_threads,name)) {
                p4_sample_threads(pids[k],name,(unsigned)p4_threads,p4_phase,uptime,
                    at>=previous_at?at-previous_at:0,
                    (double)base.numer/(double)base.denom,
                    !look || p4_n_before==0);
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
        }
        if(look)for(size_t i=0;i<sizeof(targets)/sizeof(targets[0]);i++)
            if(!(mask&(1u<<i)))printf("poster_cost: %llu s: %s missing\n",t,targets[i]);
        if(p4_threads>0) {
            memcpy(p4_before,p4_after,(size_t)p4_n_after*sizeof(p4_before[0]));
            p4_n_before=p4_n_after;
        }
        memcpy(prev,now,(size_t)count*sizeof(now[0]));
        prev_count=count;previous_at=at;
        if(look==looks)break;
        sleep(interval);
    }
    return 0;
}
