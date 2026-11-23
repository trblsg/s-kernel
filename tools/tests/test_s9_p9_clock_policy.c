#include "../../userland/src/s9_p9_clock_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const uint64_t t = UINT64_C(10000000);
    /* Hidden Clock, quiet 3s, last forwarded 100ms ago => skip. */
    assert(s9_p9_hidden_clock_skip(t,t-4000000,t-100000,false,true,true,true));
    /* Never suppress visible Home Clock or an unrelated CADisplayLink. */
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t-100000,true,true,true,true));
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t-100000,false,false,true,true));
    /* User input, unhooked SpringBoard, disabled policy all fail open. */
    assert(!s9_p9_hidden_clock_skip(t,t-100000,t-100000,false,true,true,true));
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t-100000,false,true,false,true));
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t-100000,false,true,true,false));
    /* At least one callback per second, even when hidden and idle. */
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t-1000000,false,true,true,true));
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,0,false,true,true,true));
    assert(!s9_p9_hidden_clock_skip(t,t+1,t-100000,false,true,true,true));
    assert(!s9_p9_hidden_clock_skip(t,t-4000000,t+1,false,true,true,true));
    puts("P9 hidden Clock skip policy: 10 strict cases PASS");
    return 0;
}
