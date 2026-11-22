#include "../../userland/src/s9_hi_auto_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const uint64_t t=UINT64_C(200000000);
    assert(!hi_auto_candidate(HI_AUTO_BOOT_US-1,t-10000000,51,true,true));
    assert(!hi_auto_candidate(t,t-2000000,51,true,true));
    assert(!hi_auto_candidate(t,t-10000000,30,true,true));
    assert(!hi_auto_candidate(t,t-10000000,51,false,true));
    assert(!hi_auto_candidate(t,t-10000000,51,true,false));
    assert(hi_auto_candidate(t,t-10000000,51,true,true));
    assert(hi_auto_must_abort(t,t-20000000,t-1000000,t-100000,true,true));
    assert(hi_auto_must_abort(t,t-20000000,t-20000000,t-100000,false,true));
    assert(hi_auto_must_abort(t,t-20000000,t-20000000,t-4000000,true,true));
    assert(!hi_auto_must_abort(t,t-20000000,t-20000000,t-100000,true,true));
    assert(hi_auto_next(HI_AUTO_WAIT)==HI_AUTO_WAIT);
    assert(hi_auto_next(HI_AUTO_BASELINE)==HI_AUTO_CUT1);
    assert(hi_auto_next(HI_AUTO_CUT1)==HI_AUTO_RECOVER1);
    assert(hi_auto_next(HI_AUTO_RECOVER1)==HI_AUTO_CUT2);
    assert(hi_auto_next(HI_AUTO_CUT2)==HI_AUTO_RECOVER2);
    assert(hi_auto_next(HI_AUTO_RECOVER2)==HI_AUTO_FINISHED);
    assert(hi_auto_next(HI_AUTO_FINISHED)==HI_AUTO_FINISHED);
    assert(hi_auto_cut_phase(HI_AUTO_CUT1));
    assert(hi_auto_cut_phase(HI_AUTO_CUT2));
    assert(!hi_auto_cut_phase(HI_AUTO_RECOVER1));
    assert(!hi_auto_cut_phase(HI_AUTO_FINISHED));
    puts("S9 autonomous A/B policy: boot/quiet/clock/abort/transition guards PASS");
    return 0;
}
