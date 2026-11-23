/* S9 P10: exact-source lock-screen timer policy.
 *
 * No generic CoreAnimation/WindowServer frame dropping. Only the real
 * SpringBoard CADisplayLink target+selector pairs seen in S9 logs qualify.
 * Clock: >=1 callback/second. UIKit animation manager: >=15Hz ONLY after
 * complete animation and CA-commit quiet proof, and mode 2 explicitly.
 * Fail open on scene, timing, input, owner or display uncertainty.
 */
#ifndef S9_P10_LOCK_POLICY_H
#define S9_P10_LOCK_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define S9_P10_MIN_LOCK_US UINT64_C(5000000)
#define S9_P10_MIN_INPUT_QUIET_US UINT64_C(3000000)
#define S9_P10_PROOF_FRESH_US UINT64_C(1500000)
#define S9_P10_CLOCK_FORWARD_US UINT64_C(1000000)
#define S9_P10_ANIM_FORWARD_US UINT64_C(66667)
#define S9_P10_ANIM_QUIET_US UINT64_C(8000000)

enum s9_p10_source {
    S9_P10_OTHER=0, S9_P10_PRECISE_CLOCK=1, S9_P10_UI_ANIM=2
};

static inline enum s9_p10_source
s9_p10_source(const char *target, const char *action)
{
    if(!target || !action)return S9_P10_OTHER;
    if(!strcmp(target,"SBUIPreciseClockTimer") &&
       !strcmp(action,"_onDisplayLink:"))
        return S9_P10_PRECISE_CLOCK;
    if(!strcmp(target,"UIViewInProcessAnimationManager") &&
       !strcmp(action,"_displayLinkFire:"))
        return S9_P10_UI_ANIM;
    return S9_P10_OTHER;
}

static inline bool
s9_p10_skip(uint64_t now, uint64_t input, uint64_t forwarded,
    uint64_t lock_since, uint64_t scene_probed, uint64_t animations_quiet_since,
    bool display_on, bool ui_locked, bool cover_visible, bool input_hook_ok,
    bool complete_animation_scan, bool no_animations, bool no_scene_commits,
    unsigned mode, enum s9_p10_source source)
{
    if(mode == 0 || source == S9_P10_OTHER ||
       !display_on || !ui_locked || !cover_visible || !input_hook_ok ||
       !now || !input || !forwarded || !lock_since || !scene_probed ||
       now<input || now<forwarded || now<lock_since || now<scene_probed ||
       now-input<S9_P10_MIN_INPUT_QUIET_US ||
       now-lock_since<S9_P10_MIN_LOCK_US ||
       now-scene_probed>S9_P10_PROOF_FRESH_US)
        return false;
    if(source==S9_P10_PRECISE_CLOCK)
        return now-forwarded<S9_P10_CLOCK_FORWARD_US;
    if(mode<2 || source!=S9_P10_UI_ANIM ||
       !complete_animation_scan || !no_animations || !no_scene_commits ||
       !animations_quiet_since || now<animations_quiet_since ||
       now-animations_quiet_since<S9_P10_ANIM_QUIET_US)
        return false;
    return now-forwarded<S9_P10_ANIM_FORWARD_US;
}
#endif
