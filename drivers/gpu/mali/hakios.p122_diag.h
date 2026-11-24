/* SPDX-License-Identifier: MIT
 * P12.2 read-only causal instrumentation helpers.
 * No PMU, PLL, frequency, voltage, frame or scheduler writes.
 */
#ifndef HAKIOS_P122_DIAG_H
#define HAKIOS_P122_DIAG_H
#include <stdint.h>
static inline int s9_p122_mode_valid(int mode) {
    return mode >= 0 && mode <= 2;
}
/* GPU queue / execution latency buckets: <=.25,.5,1,2,4,8,16ms, >16ms.
 * Histogram counters (rather than per-job logs) keep the worker bounded. */
static inline unsigned s9_p122_bucket_us(uint64_t us) {
    static const uint32_t limit[7] = {250,500,1000,2000,4000,8000,16000};
    for (unsigned i=0; i<7; ++i)
        if (us <= limit[i]) return i;
    return 7;
}
/* P12.3: pure lower-only frame-floor experiment, fail open.
 * Times are monotonic milliseconds; all validity proofs must be current.
 * Never lower the load governor's own requested frequency or thermal cap. */
static inline int s9_p123_trial_active(uint64_t now,uint64_t until) {
    return until != 0 && now < until;
}
static inline int s9_p123_floor_eligible(
    int armed, int display_on, int input_quiet, int thermal_fresh,
    int32_t gpu_mc, uint32_t streak, uint64_t gap_ms,
    uint64_t age_ms, uint32_t load)
{
    return armed && display_on == 1 && input_quiet && thermal_fresh &&
        gpu_mc >= 0 && gpu_mc < 72000 && streak >= 6 &&
        gap_ms >= 80 && gap_ms <= 150 && age_ms <= 150 && load <= 8;
}
static inline uint32_t s9_p123_frame_streak(uint32_t old, uint64_t gap_ms)
{
    if(gap_ms < 80 || gap_ms > 150) return 0;
    return old < 32 ? old + 1 : 32;
}
#endif
