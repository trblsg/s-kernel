/* S9 Clock owner-verified inactive-icon policy (host-testable C11/C++17).
 * Fail OPEN on ambiguous runtime shape, scene, user input, a visible icon,
 * or an owner transition. Unlike the 20s experiment, this may remain armed
 * long term, but forwards at least one callback per second. */
#ifndef S9_HI_VERIFIED_POLICY_H
#define S9_HI_VERIFIED_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#define HI_VERIFIED_STABLE_US UINT64_C(8000000)
#define HI_VERIFIED_TOUCH_US UINT64_C(3000000)
#define HI_VERIFIED_FORWARD_US UINT64_C(1000000)
enum hi_view_status {
    HI_VIEW_UNKNOWN=0, HI_VIEW_VISIBLE=1, HI_VIEW_DETACHED=2,
    HI_VIEW_HIDDEN=3, HI_VIEW_OUTSIDE=4
};
static inline bool
hi_view_proven_offscreen(enum hi_view_status v)
{
    return v==HI_VIEW_DETACHED || v==HI_VIEW_HIDDEN || v==HI_VIEW_OUTSIDE;
}
static inline bool
hi_verified_should_skip(uint64_t now,uint64_t last_touch,uint64_t last_forward,
    uint64_t verified_since,enum hi_view_status status,
    bool on_main_home,bool hook_ok,bool source_is_clock,bool policy_enabled)
{
    if(!policy_enabled || !source_is_clock || !hook_ok || !on_main_home ||
       !hi_view_proven_offscreen(status) || !verified_since ||
       !last_forward || now<verified_since || now<last_touch ||
       now<last_forward || now-verified_since<HI_VERIFIED_STABLE_US ||
       now-last_touch<HI_VERIFIED_TOUCH_US)return false;
    return now-last_forward<HI_VERIFIED_FORWARD_US;
}
#endif
