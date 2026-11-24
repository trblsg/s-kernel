#ifndef HAKIOS_P12_THERMAL_H
#define HAKIOS_P12_THERMAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * P12 low-risk optional thermal experiment: never lower a frame-rate cap,
 * drop a frame, alter touch latency policy, turn off a core or write voltages.
 * Only after stable no-finger 60Hz presentation, a fresh TMU and no late frame
 * may the BIG Scion work target rise from 60 to 70 percent.
 */
static inline bool
s9_p12_cpu_eligible(uint32_t mode, uint32_t governor_policy,
    uint32_t consecutive_ontime_frames, uint32_t finger_count,
    bool touch_recent, bool late_cooldown, int display_request,
    bool temp_fresh, int32_t big_millidegrees)
{
    return mode == 1 && governor_policy == 2 &&
        consecutive_ontime_frames >= 30 && finger_count == 0 &&
        !touch_recent && !late_cooldown && display_request == 1 &&
        temp_fresh && big_millidegrees >= 68000 &&
        big_millidegrees < 83000;
}

static inline uint32_t
s9_p12_cpu_rated_load(uint32_t raw_load, bool eligible)
{
    /* Never increase the frequency demand above stock under this mode. */
    return eligible ? (uint32_t)((uint64_t)raw_load * 60 / 70) : raw_load;
}

#endif
