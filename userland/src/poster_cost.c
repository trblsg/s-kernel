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
static const char *targets[]={"backboardd","CollectionsPoster","PosterBoard","SpringBoard","chronod"};
static struct snap prev[MAX_MATCHES],now[MAX_MATCHES];
static int prev_count;
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
        int count=0;unsigned mask=0;
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
            if(!look)continue;
            const uint64_t used=info.pti_total_user+info.pti_total_system;
            const uint64_t prior=old?old->info.pti_total_user+old->info.pti_total_system:0;
            const double cpu=(old && used>=prior && elapsed>0)?100.0*(double)(used-prior)/elapsed:0;
            const int32_t faults=old?info.pti_faults-old->info.pti_faults:0;
            const int32_t pageins=old?info.pti_pageins-old->info.pti_pageins:0;
            const int32_t csw=old?info.pti_csw-old->info.pti_csw:0;
            const int32_t mach=old?info.pti_syscalls_mach-old->info.pti_syscalls_mach:0;
            const int32_t unix=old?info.pti_syscalls_unix-old->info.pti_syscalls_unix:0;
            printf("poster_cost: %llu s: %s[%d] cpu=%.1f%% rss=%lluKiB vm=%lluMiB faults=%d pageins=%d csw=%d mach=%d unix=%d %s\n",
                t,name,pids[k],cpu,(unsigned long long)(info.pti_resident_size>>10),
                (unsigned long long)(info.pti_virtual_size>>20),
                faults<0?0:faults,pageins<0?0:pageins,csw<0?0:csw,mach<0?0:mach,unix<0?0:unix,
                old?"sample":"new-pid");
        }
        if(look)for(size_t i=0;i<sizeof(targets)/sizeof(targets[0]);i++)
            if(!(mask&(1u<<i)))printf("poster_cost: %llu s: %s missing\n",t,targets[i]);
        memcpy(prev,now,(size_t)count*sizeof(now[0]));
        prev_count=count;previous_at=at;
        if(look==looks)break;
        sleep(interval);
    }
    return 0;
}
