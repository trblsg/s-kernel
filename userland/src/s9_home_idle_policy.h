/* S9 Home Idle one-boot A/B: pure policy, also compiled by the host test.
 * Every risky condition fails OPEN (forwards the display-link callback).
 * This is a 30-second manual experiment, NOT a production rendering throttle. */
#ifndef S9_HOME_IDLE_POLICY_H
#define S9_HOME_IDLE_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define S9_HI_INPUT_GRACE_US UINT64_C(2500000)
#define S9_HI_FORWARD_PERIOD_US UINT64_C(1000000)
#define S9_HI_GATE_US UINT64_C(30000000)
/* Clock's _timerFired: stays live at 10Hz on Home0 even with 9 other
 * animation keys. Only this EXACT source can bypass the broad layer-
 * animation gate in a manually armed and timed causal A/B. */
static inline bool
s9_hi_clock_source(const char *target, const char *action)
{
    return target && action &&
        strcmp(target, "SBHClockApplicationIconImageView") == 0 &&
        strcmp(action, "_timerFired:") == 0;
}
static inline bool
s9_hi_skip(uint64_t now, uint64_t deadline, uint64_t last_input,
    uint64_t last_forward, bool manual_primary, bool home_visible,
    bool event_hook_ok, bool no_visible_animation)
{
    if (!manual_primary || !home_visible || !event_hook_ok ||
        !no_visible_animation || deadline == 0 || now >= deadline ||
        now < last_input || now - last_input <= S9_HI_INPUT_GRACE_US ||
        last_forward == 0 || now < last_forward)
        return false;
    return now - last_forward < S9_HI_FORWARD_PERIOD_US;
}
#endif
