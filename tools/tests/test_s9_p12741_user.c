/* Compile the actual user header with minimal portable API stubs, never
 * represent this as a native iOS SDK build. Verifies hot-tail logging state
 * and exact eight-word payload without serial or policy writes. */
#include <assert.h>
#include "../../userland/src/p1274_user.h"
static uint64_t fake_now=1000000000ull, payload[8];
static int fake_state=0, writes=0;
uint64_t mach_absolute_time(void){return fake_now;}
int mach_timebase_info(mach_timebase_info_data_t *info){info->numer=info->denom=1;return 0;}
int sysctlbyname(const char *name,void *oldp,size_t *oldlenp,void *newp,size_t newlen){
    if(!strcmp(name,"kern.bootargs")){
        const char args[]="hi=2 p1274=1";
        assert(oldp && oldlenp && *oldlenp>=sizeof(args));
        memcpy(oldp,args,sizeof(args));*oldlenp=sizeof(args);return 0;
    }
    if(!strcmp(name,"kern.hakios.p12741_user_state")){
        assert(oldp && oldlenp && *oldlenp==sizeof(int));
        memcpy(oldp,&fake_state,sizeof(fake_state));return 0;
    }
    if(!strcmp(name,"kern.hakios.p1274_user_event")){
        assert(newp && newlen==sizeof(payload));
        memcpy(payload,newp,sizeof(payload));writes++;return 0;
    }
    errno=ENOENT;return -1;
}
int main(void){
    assert(p1274_user_mode()==0 && p1274_user_observing());
    assert(p1274_emit(2,1,2,3,4,5,6,7)==0);
    assert(writes==1 && payload[0]==2);
    for(unsigned i=1;i<8;i++)assert(payload[i]==i);
    fake_now+=300000000ull;fake_state=2;
    assert(p1274_user_mode()==2 && p1274_user_observing());
    assert(p1274_emit(3,9,8,7,6,5,4,3)==0 && writes==2);
    fake_now+=300000000ull;fake_state=3;
    assert(p1274_user_mode()==2 && !p1274_user_observing());
    assert(p1274_emit(2,1,2,3,4,5,6,7)==-1 && writes==2);
    assert(p1274_stage_begin().at==0);
    puts("P1274.1 portable actual user header, payload and suppression-only PASS");
    return 0;
}
