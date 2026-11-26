/* P12.7.4 optional user telemetry. No persistent timer, no policy writes.
 * Cache read on natural calls, one query at most per 250 ms per TU.
 * Binary event ABI: 8 uint64_t, kernel stamps producer PID/task/TID.
 * Evidence is advisory; denied/dropped reports are never treated as zero. */
#ifndef S9_P1274_USER_H
#define S9_P1274_USER_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
static inline int p1274_user_boot(void)
{
    static int cached=-1;
    int old=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(old>=0)return old;
    char args[4096]={0};size_t n=sizeof(args)-1;int enabled=0;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0)==0){
        args[n<sizeof(args)?n:sizeof(args)-1]=0;
        const char *s=args;
        while((s=strstr(s,"p1274=1"))!=NULL){
            if((s==args || s[-1]==' ') && (s[7]==0 || s[7]==' ')){enabled=1;break;}
            s++;
        }
    }
    __atomic_store_n(&cached,enabled,__ATOMIC_RELEASE);
    return enabled;
}
static inline int p1274_user_mode(void)
{
    if(!p1274_user_boot())return -1;
    static uint64_t next=0;
    static int mode=-1;
    uint64_t now=mach_absolute_time(),old=__atomic_load_n(&next,__ATOMIC_RELAXED);
    if(now>=old){
        mach_timebase_info_data_t t={0};
        mach_timebase_info(&t);
        uint64_t ttl=t.numer?250000000ull*t.denom/t.numer:1;
        if(__atomic_compare_exchange_n(&next,&old,now+ttl,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
            int v=-1;size_t n=sizeof(v);
            if(sysctlbyname("kern.hakios.p1274_metal_log_mode",&v,&n,NULL,0)!=0 ||
               n!=sizeof(v) || (v!=0 && v!=2))v=-1;
            __atomic_store_n(&mode,v,__ATOMIC_RELEASE);
        }
    }
    return __atomic_load_n(&mode,__ATOMIC_ACQUIRE);
}
static inline int p1274_emit(uint64_t kind,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g)
{
    if(p1274_user_mode()<0)return -1;
    uint64_t row[8]={kind,a,b,c,d,e,f,g};
    int e=sysctlbyname("kern.hakios.p1274_user_event",NULL,NULL,row,sizeof(row));
    if(e){static int warned; if(!__atomic_exchange_n(&warned,1,__ATOMIC_RELAXED))fprintf(stderr,"p1274 user event unavailable errno=%d\n",errno);}
    return e;
}
static inline uint64_t p1274_thread_ns(void)
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec t;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
#endif
    return UINT64_MAX; /* missing, not zero */
}
struct p1274_stage {uint64_t at,cpu;};
static inline struct p1274_stage p1274_stage_begin(void)
{
    struct p1274_stage t={0,UINT64_MAX};
    if(p1274_user_mode()>=0){t.at=mach_absolute_time();t.cpu=p1274_thread_ns();}
    return t;
}
static inline void p1274_stage_end(struct p1274_stage t,unsigned stage,unsigned ok)
{
    if(!t.at)return;
    uint64_t now=mach_absolute_time(),cpu=p1274_thread_ns();
    (void)p1274_emit(3,stage,now-t.at,
        cpu==UINT64_MAX || t.cpu==UINT64_MAX || cpu<t.cpu?UINT64_MAX:cpu-t.cpu,
        ok,t.at,0,0);
}
#endif
