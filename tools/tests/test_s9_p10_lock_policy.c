#include "../../userland/src/s9_p10_lock_policy.h"
#include <assert.h>
#include <stdio.h>

static bool try_skip(enum s9_p10_source source,unsigned mode,
    bool on,bool locked,bool cover,bool hook,bool complete,bool noanim,
    bool nocommit,uint64_t input,uint64_t last,uint64_t lock_since,
    uint64_t seen,uint64_t anim_since)
{
    const uint64_t now=UINT64_C(20000000);
    return s9_p10_skip(now,input,last,lock_since,seen,anim_since,
        on,locked,cover,hook,complete,noanim,nocommit,mode,source);
}
#define TRY(type,mode,on,lock,cover,hook,all,empty,commits,input,last,locked_at,proof,quiet) \
    try_skip((type),(mode),(on),(lock),(cover),(hook),(all),(empty),(commits), \
             (input),(last),(locked_at),(proof),(quiet))
int main(void)
{
    const uint64_t now=UINT64_C(20000000);
    assert(s9_p10_source("SBUIPreciseClockTimer","_onDisplayLink:")==S9_P10_PRECISE_CLOCK);
    assert(s9_p10_source("UIViewInProcessAnimationManager","_displayLinkFire:")==S9_P10_UI_ANIM);
    assert(s9_p10_source("SBUIPreciseClockTimer","_timerFired:")==S9_P10_OTHER);
    assert(s9_p10_source("CollectionsPoster","_onDisplayLink:")==S9_P10_OTHER);
    assert(s9_p10_source(NULL,"_onDisplayLink:")==S9_P10_OTHER);
    assert(s9_p10_source("SBUIPreciseClockTimer",NULL)==S9_P10_OTHER);
    /* A proven, input-quiet, 5s stable and fresh locked cover sheet:
     * only the exact clock source may be skipped, at most 1 second. */
    assert(TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,0,1,1,1,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,0,1,1,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,0,1,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,0,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,0,0,0,0,now-4000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,1,0,0,0,now-2000000,
        now-300000,now-6000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,1,0,0,0,now-4000000,
        now-300000,now-2000000,now-500000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,1,0,0,0,now-4000000,
        now-300000,now-6000000,now-1800000,0));
    assert(!TRY(S9_P10_PRECISE_CLOCK,1,1,1,1,1,0,0,0,now-4000000,
        now-1000000,now-6000000,now-500000,0)); /* >=1Hz forward */
    assert(!TRY(S9_P10_OTHER,2,1,1,1,1,1,1,1,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000)); /* never global */
    /* UIKit animation manager ONLY mode2 + no animations or commits for 8s. */
    assert(!TRY(S9_P10_UI_ANIM,1,1,1,1,1,1,1,1,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,1,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,0,1,1,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,0,1,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,0,now-4000000,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,1,now-4000000,
        now-20000,now-10000000,now-100000,now-1000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,1,now-4000000,
        now-80000,now-10000000,now-100000,now-9000000)); /* >=15Hz */
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,1,now+1,
        now-20000,now-10000000,now-100000,now-9000000));
    assert(!TRY(S9_P10_UI_ANIM,2,1,1,1,1,1,1,1,now-4000000,
        now+1,now-10000000,now-100000,now-9000000));
    puts("P10 lockscreen SOURCE-only policies PASS: exact Clock, 15Hz guarded UIKit, scene/animation/input fail-open");
    return 0;
}
