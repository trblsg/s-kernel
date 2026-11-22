#include "../../userland/src/s9_home_idle_policy.h"
#include <assert.h>
#include <stdio.h>
static void check(uint64_t now, uint64_t deadline, uint64_t input,
    uint64_t forwarded, bool primary, bool visible, bool hooks, bool animation,
    bool expected)
{
    assert(s9_hi_skip(now, deadline, input, forwarded, primary, visible,
            hooks, !animation) == expected);
}
int main(void)
{
    const uint64_t t = UINT64_C(100000000);
    /* Only a user-armed, stable, visible primary Home can be gated. */
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 100000,
        true, true, true, false, true);
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 100000,
        false, true, true, false, false);
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 100000,
        true, false, true, false, false);
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 100000,
        true, true, false, false, false);
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 100000,
        true, true, true, true, false);
    /* Interactions and animations must not be delayed. */
    check(t, t + S9_HI_GATE_US, t - 200000, t - 100000,
        true, true, true, false, false);
    check(t, t + S9_HI_GATE_US, t - 2500000, t - 100000,
        true, true, true, false, false);
    /* A real frame is always forwarded once a second. */
    check(t, t + S9_HI_GATE_US, t - 5000000, t - 1200000,
        true, true, true, false, false);
    /* No skip at expiry, clock rollback, or if no callback was sent yet. */
    check(t, t, t - 5000000, t - 100000,
        true, true, true, false, false);
    check(t, t + S9_HI_GATE_US, t + 1, t - 100000,
        true, true, true, false, false);
    check(t, t + S9_HI_GATE_US, t - 5000000, t + 1,
        true, true, true, false, false);
    check(t, t + S9_HI_GATE_US, t - 5000000, 0,
        true, true, true, false, false);
    puts("s9-home-idle-policy: 11 guarded cases OK");
    return 0;
}
