/* P9 narrowly targeted Clock icon invalidation policy.
 * An SBHClockApplicationIconImageView cannot be visible while the Home
 * window is not being shown. Reduce only ITS CADisplayLink callbacks;
 * never suppress CoreAnimation or GL global presents. Maintain one
 * refresh per second so date/seconds update when returning to Home.
 * Input, ambiguity, scene change and clock changes all fail OPEN.
 */
#ifndef S9_P9_CLOCK_POLICY_H
#define S9_P9_CLOCK_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#define S9_P9_NO_INPUT_US UINT64_C(3000000)
#define S9_P9_CLOCK_MIN_FORWARD_US UINT64_C(1000000)
static inline bool
s9_p9_hidden_clock_skip(uint64_t now, uint64_t last_input,
    uint64_t last_forward, bool home_visible, bool clock_target,
    bool hook_ok, bool enabled)
{
    if (!enabled || !clock_target || !hook_ok || home_visible ||
        last_input == 0 || last_forward == 0 ||
        now < last_input || now < last_forward ||
        now - last_input < S9_P9_NO_INPUT_US)
        return false;
    return now - last_forward < S9_P9_CLOCK_MIN_FORWARD_US;
}
#endif
