/* Bounded in-process diagnostic only. No task_for_pid, explicit suspension,
 * user-stack memory walks, scheduling changes, or private codec hooks.
 * Sample hottest observed threads' PC/LR; these are not complete stacks.
 * Only naturally busy, armed OFF capture; no sampling in idle energy tests. */
#ifndef S9_P12752_SAMPLER_H
#define S9_P12752_SAMPLER_H
#include "p1274_user.h"
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
static inline int p12752_user_boot(void){
    if(!p12751_user_boot())return 0;
    static int cached=-1;int v=__atomic_load_n(&cached,__ATOMIC_ACQUIRE);
    if(v>=0)return v;
    char args[4096]={0};size_t n=sizeof(args)-1;v=0;
    if(sysctlbyname("kern.bootargs",args,&n,NULL,0)==0 && n<sizeof(args)){
        args[n]=0;const char *s=args;
        while((s=strstr(s,"p12752=1"))!=NULL){
            if((s==args || s[-1]==' ') && (s[8]==0 || s[8]==' ')){v=1;break;}s++;
        }
    }
    __atomic_store_n(&cached,v,__ATOMIC_RELEASE);return v;
}
static inline int p12752_window(void){
    int v=0;size_t n=sizeof(v);
    return sysctlbyname("kern.hakios.p12752_sample_window",&v,&n,NULL,0)==0 && n==sizeof(v) && v==1;
}
static inline uint64_t p12752_uptime_ns(void){
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
static inline void p12752_addr(char *buf,size_t n,uintptr_t addr){
    Dl_info info={0};
    if(addr && dladdr((void*)addr,&info) && info.dli_fname && info.dli_fbase){
        const char *name=strrchr(info.dli_fname,'/');
        snprintf(buf,n,"%s+0x%llx",name?name+1:info.dli_fname,
            (unsigned long long)(addr-(uintptr_t)info.dli_fbase));
    }else snprintf(buf,n,"unresolved");
}
static inline void *p12752_sample_main(void *arg){
    (void)arg;
    const char *name=getprogname();
    if(!name)return NULL;
    pthread_setname_np("p12752-sample");
    uint64_t now=p12752_uptime_ns();
    if(now<290000000000ull){
        uint64_t wait=290000000000ull-now;
        struct timespec t={(time_t)(wait/1000000000ull),(long)(wait%1000000000ull)};
        while(nanosleep(&t,&t)!=0 && errno==EINTR){}
    }
    int profile=0;size_t size=sizeof(profile);
    if(sysctlbyname("kern.hakios.p12752_profile",&profile,&size,NULL,0)!=0 ||
       size!=sizeof(profile) || profile<1 || profile>4 || !p1274_user_observing())return NULL;
    unsigned polls=0,looks=0,kept=0,failed=0;
    mach_port_t self=mach_thread_self();
    while(p12752_uptime_ns()<390000000000ull && polls++<120 && looks<20){
        if(!p1274_user_observing())break;
        if(!p12752_window()){usleep(1000000);continue;}
        looks++;
        thread_act_array_t threads=NULL;mach_msg_type_number_t count=0;
        if(task_threads(mach_task_self(),&threads,&count)!=KERN_SUCCESS){failed++;usleep(250000);continue;}
        struct {thread_t port;integer_t usage;} top[2]={{0,0},{0,0}};
        if(count<=128){
            for(mach_msg_type_number_t i=0;i<count;i++){
                if(threads[i]==self)continue;
                thread_basic_info_data_t info;mach_msg_type_number_t n=THREAD_BASIC_INFO_COUNT;
                if(thread_info(threads[i],THREAD_BASIC_INFO,(thread_info_t)&info,&n)!=KERN_SUCCESS ||
                    (info.flags&TH_FLAGS_IDLE) || info.cpu_usage<=0)continue;
                if(info.cpu_usage>top[0].usage){top[1]=top[0];top[0].port=threads[i];top[0].usage=info.cpu_usage;}
                else if(info.cpu_usage>top[1].usage){top[1].port=threads[i];top[1].usage=info.cpu_usage;}
            }
            for(unsigned i=0;i<2;i++){
                if(!top[i].port || !p12752_window())continue;
                arm_thread_state64_t state;mach_msg_type_number_t n=ARM_THREAD_STATE64_COUNT;
                thread_identifier_info_data_t ident;mach_msg_type_number_t ni=THREAD_IDENTIFIER_INFO_COUNT;
                if(thread_info(top[i].port,THREAD_IDENTIFIER_INFO,(thread_info_t)&ident,&ni)!=KERN_SUCCESS ||
                   thread_get_state(top[i].port,ARM_THREAD_STATE64,(thread_state_t)&state,&n)!=KERN_SUCCESS){failed++;continue;}
                char pc[128],lr[128],line[440];
                p12752_addr(pc,sizeof(pc),(uintptr_t)arm_thread_state64_get_pc(state));
                p12752_addr(lr,sizeof(lr),(uintptr_t)arm_thread_state64_get_lr(state));
                int len=snprintf(line,sizeof(line),"P12752 sample at=%llu pid=%d name=%s tid=%llu usage=%d pc=%s lr=%s\n",
                    (unsigned long long)mach_absolute_time(),getpid(),name,(unsigned long long)ident.thread_id,
                    (int)top[i].usage,pc,lr);
                if(len>0 && (size_t)len<sizeof(line)){write(2,line,(size_t)len);kept++;}
            }
        }else failed++;
        for(mach_msg_type_number_t i=0;i<count;i++)mach_port_deallocate(mach_task_self(),threads[i]);
        if(threads)vm_deallocate(mach_task_self(),(vm_address_t)threads,count*sizeof(thread_t));
        usleep(250000);
    }
    mach_port_deallocate(mach_task_self(),self);
    {char line[160];int n=snprintf(line,sizeof(line),"P12752 sample_done pid=%d looks=%u kept=%u failures=%u\n",getpid(),looks,kept,failed);
        if(n>0 && (size_t)n<sizeof(line))write(2,line,(size_t)n);}
    return NULL;
}
static inline void p12752_start_sampler(int media){
    if(!p12752_user_boot())return;
    const char *name=getprogname();if(!name)return;
    if(media?strcmp(name,"mediaserverd")!=0:
       (strcmp(name,"PosterBoard") && strcmp(name,"CollectionsPoster") &&
        strcmp(name,"PridePosterExtension") && strcmp(name,"EmojiPosterExtension") &&
        strcmp(name,"GradientPosterExtension") && strcmp(name,"UnityPosterExtension")))return;
    static int started;
    if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
    pthread_t thread;
    if(pthread_create(&thread,NULL,p12752_sample_main,NULL)==0)pthread_detach(thread);
}
#endif
