/* SPDX-License-Identifier: MIT
 * S9 P4 CPU idle instrumentation: scheduler-idle ticks, NOT CPD/SICD/WFI.
 * Portable arithmetic tested under strict C11/C++17 with UBSan.
 */
#ifndef S9_P4_IDLE_MATH_H
#define S9_P4_IDLE_MATH_H
#include <stdint.h>
#define S9_P4_CORE_COUNT 8
#define S9_P4_NO_SAMPLE 255u

/* Counter is monotonic mach_absolute_time ticks, same units as span.
 * A removed/offline core is not counted as either busy or idle.
 * A counter reset, counter wrap or absent sample is explicitly unknown. */
static inline uint32_t
s9_p4_idle_percent(uint64_t previous, uint64_t current, uint64_t span,
                   int valid, int online)
{
    uint64_t delta;
    if (!valid || !online || span == 0 || current < previous)
        return S9_P4_NO_SAMPLE;
    delta = current - previous;
    if (delta >= span)
        return 100u;
    /* Divide before multiply only when multiplication may overflow. */
    if (delta <= UINT64_MAX / 100u)
        return (uint32_t)((delta * 100u) / span);
    return (uint32_t)(delta / (span / 100u + (span % 100u != 0)));
}

static inline uint32_t
s9_p4_gap_bucket(uint64_t delta_ticks, uint64_t ms17, uint64_t ms50,
                 uint64_t ms120)
{
    if (delta_ticks == 0)
        return 4u; /* no previous frame: unknown, not >=120ms */
    if (delta_ticks < ms17) return 0u;
    if (delta_ticks < ms50) return 1u;
    if (delta_ticks < ms120) return 2u;
    return 3u;
}

#endif
