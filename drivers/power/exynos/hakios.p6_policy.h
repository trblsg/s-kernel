/* SPDX-License-Identifier: MIT
 * S9 P6.3 offline policy classifier: NOT an actuator. All quantities
 * are Exynos9810-specific; do not transplant Exynos990 clock/PMU values.
 */
#ifndef HAKIOS_S9_P6_POLICY_H
#define HAKIOS_S9_P6_POLICY_H
#include <stdint.h>
struct s9_p6_shadow {
    unsigned big_hot, gpu_hot, park_candidate;
    unsigned changed;
};
static inline int s9_p6_temp_ok(int32_t mc)
{
    return mc >= 0 && mc <= 125000;
}
static inline struct s9_p6_shadow
s9_p6_shadow_eval(struct s9_p6_shadow s, int32_t big_mc, int32_t gpu_mc,
    unsigned big_busy, unsigned little_busy, unsigned touches,
    unsigned frames, unsigned frame_gap_ms)
{
    unsigned prev_big=s.big_hot, prev_gpu=s.gpu_hot, prev_park=s.park_candidate;
    /* Thermal shadow is two hysteresis states, not Samsung IPA. Unknown
     * sensors HOLD the previous level, never imply that cooling happened. */
    if (s9_p6_temp_ok(big_mc)) {
        if (big_mc >= 96000) s.big_hot=2;
        else if (big_mc >= 83000 && s.big_hot<1) s.big_hot=1;
        else if (big_mc <= 78000) s.big_hot=0;
        else if (big_mc <= 91000 && s.big_hot==2) s.big_hot=1;
    }
    if (s9_p6_temp_ok(gpu_mc)) {
        if (gpu_mc >= 95000) s.gpu_hot=2;
        else if (gpu_mc >= 88000 && s.gpu_hot<1) s.gpu_hot=1;
        else if (gpu_mc <= 83000) s.gpu_hot=0;
        else if (gpu_mc <= 90000 && s.gpu_hot==2) s.gpu_hot=1;
    }
    /* A low-load snapshot is only a parking *candidate*, not proof that
     * taking M3s out will meet a real UI deadline. Ignore uninitialized
     * load (UINT32_MAX). A 10Hz static compositor is not 60Hz motion. */
    s.park_candidate = big_busy <= 25 && little_busy <= 70 &&
        touches == 0 && (frames == 0 ||
        (frames <= 60 && frame_gap_ms >= 70)) &&
        big_busy != UINT32_MAX && little_busy != UINT32_MAX;
    s.changed = (prev_big!=s.big_hot || prev_gpu!=s.gpu_hot ||
        prev_park!=s.park_candidate);
    return s;
}
#endif
