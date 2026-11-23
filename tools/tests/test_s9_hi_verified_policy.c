#include "../../userland/src/s9_hi_verified_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint64_t t=UINT64_C(22000000);
    for(int v=HI_VIEW_DETACHED;v<=HI_VIEW_OUTSIDE;v++)
        assert(hi_view_proven_offscreen((enum hi_view_status)v));
    assert(!hi_view_proven_offscreen(HI_VIEW_UNKNOWN));
    assert(!hi_view_proven_offscreen(HI_VIEW_VISIBLE));
    assert(hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_HIDDEN,true,true,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_UNKNOWN,true,true,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_VISIBLE,true,true,true,true));
    assert(!hi_verified_should_skip(t,20000000,21500000,1000000,HI_VIEW_HIDDEN,true,true,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,20000000,HI_VIEW_HIDDEN,true,true,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_HIDDEN,false,true,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_HIDDEN,true,false,true,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_HIDDEN,true,true,false,true));
    assert(!hi_verified_should_skip(t,100000,21500000,1000000,HI_VIEW_HIDDEN,true,true,true,false));
    assert(!hi_verified_should_skip(t,100000,18000000,1000000,HI_VIEW_HIDDEN,true,true,true,true));
    puts("S9 Clock strict visibility fail-open policy C11/C++17 PASS");
    return 0;
}
