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
#include <sys/resource.h>
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
static inline int p12741_user_state(void)
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
            if(sysctlbyname("kern.hakios.p12741_user_state",&v,&n,NULL,0)!=0 ||
               n!=sizeof(v) || (v!=0 && v!=2 && v!=3))v=-1;
            __atomic_store_n(&mode,v,__ATOMIC_RELEASE);
        }
    }
    return __atomic_load_n(&mode,__ATOMIC_ACQUIRE);
}
static inline int p1275_user_boot(void){
    if(!p1274_user_boot())return 0;
    static int cached=-1;
    int old=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);if(old>=0)return old;
    char args[4096]={0};size_t n=sizeof(args)-1;int enabled=0;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0)==0 && n<sizeof(args)){
        args[n]=0;const char *s=args;
        while((s=strstr(s,"p1275=1"))!=NULL){
            if((s==args || s[-1]==' ') && (s[7]==0 || s[7]==' ')){enabled=1;break;}s++;
        }
    }
    __atomic_store_n(&cached,enabled,__ATOMIC_RELEASE);return enabled;
}
static inline int p12751_user_boot(void){
    if(!p1275_user_boot())return 0;
    static int cached=-1;int old=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(old>=0)return old;
    char args[4096]={0};size_t n=sizeof(args)-1;int enabled=0;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0)==0 && n<sizeof(args)){
        args[n]=0;const char *s=args;
        while((s=strstr(s,"p12751=1"))!=NULL){
            if((s==args || s[-1]==' ') && (s[8]==0 || s[8]==' ')){enabled=1;break;}s++;
        }
    }
    __atomic_store_n(&cached,enabled,__ATOMIC_RELEASE);return enabled;
}
static inline int p12751_boot_mode(void){
    if(!p12751_user_boot())return -1;
    static int cached=-2;int old=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(old>=-1)return old;
    int v=-1;size_t n=sizeof(v);
    if(sysctlbyname("kern.hakios.p12751_boot_latch",&v,&n,NULL,0)!=0 || n!=sizeof(v) || v<0 || v>7)v=-1;
    /* All callers run after PID1; absent latch conservatively stays stock. */
    __atomic_store_n(&cached,v,__ATOMIC_RELEASE);return v;
}
static inline unsigned p1275_mode(void){
    if(!p1275_user_boot())return 0;
    int v=0;size_t n=sizeof(v);
    return sysctlbyname("kern.hakios.p1275_mode",&v,&n,NULL,0)==0 && n==sizeof(v) && v>=1 && v<=16?(unsigned)v:0;
}
static inline unsigned p1275_flags(void){unsigned m=p1275_mode();return m?m-1:0;}
static inline unsigned p1275_cached_flags(void){
    static uint64_t next;static unsigned flags;
    uint64_t now=mach_absolute_time(),old=__atomic_load_n(&next,__ATOMIC_RELAXED);
    if(now>=old){mach_timebase_info_data_t t={0};mach_timebase_info(&t);
        uint64_t ttl=t.numer?250000000ull*t.denom/t.numer:1;
        if(__atomic_compare_exchange_n(&next,&old,now+ttl,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED))
            __atomic_store_n(&flags,p1275_flags(),__ATOMIC_RELEASE);
    }return __atomic_load_n(&flags,__ATOMIC_ACQUIRE);
}
static inline int p1274_user_mode(void){int s=p12741_user_state();return s==3?2:s;}
static inline int p1274_user_observing(void){int s=p12741_user_state();return s==0 || s==2;}
static inline int p1274_emit(uint64_t kind,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g)
{
    if(!p1274_user_observing())return -1;
    uint64_t row[8]={kind,a,b,c,d,e,f,g};
    int rc=sysctlbyname("kern.hakios.p1274_user_event",NULL,NULL,row,sizeof(row));
    if(rc){static int warned; if(!__atomic_exchange_n(&warned,1,__ATOMIC_RELAXED))fprintf(stderr,"p1274 user event unavailable errno=%d\n",errno);}
    return rc;
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
/* Process-wide deltas, explicitly NOT the opener thread's own CPU. */
struct p1275_usage {uint64_t user,sys,minflt,majflt;int valid;};
static inline struct p1275_usage p1275_usage_now(void){
    struct p1275_usage s={0,0,0,0,0};struct rusage r;
    if(getrusage(RUSAGE_SELF,&r)==0){
        s.user=(uint64_t)r.ru_utime.tv_sec*1000000ull+r.ru_utime.tv_usec;
        s.sys=(uint64_t)r.ru_stime.tv_sec*1000000ull+r.ru_stime.tv_usec;
        s.minflt=(uint64_t)r.ru_minflt;s.majflt=(uint64_t)r.ru_majflt;s.valid=1;
    }return s;
}
static inline void p1275_usage_end(struct p1275_usage a,unsigned stage,uint64_t start){
    if(!a.valid)return;
    struct p1275_usage b=p1275_usage_now();
    if(b.valid && b.user>=a.user && b.sys>=a.sys && b.minflt>=a.minflt && b.majflt>=a.majflt)
        (void)p1274_emit(6,stage,b.user-a.user,b.sys-a.sys,b.minflt-a.minflt,b.majflt-a.majflt,start,1);
}
static inline struct p1274_stage p1274_stage_begin(void)
{
    struct p1274_stage t={0,UINT64_MAX};
    if(p1274_user_observing()){t.at=mach_absolute_time();t.cpu=p1274_thread_ns();}
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
