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

/* P12.4 lower-only experiments. Never change a ceiling or hard thermal limit.
 * A lower floor is valid only for exact supported levels. */
static inline bool s9_p124_temp_ok(bool arm,uint64_t age,int32_t big,int32_t lit,int32_t gpu)
{
    return age<=2000 && big>=0 && lit>=0 && gpu>=0 &&
        big<(arm?66000:76000) && lit<(arm?65000:70000) && gpu<(arm?66000:72000);
}
/* P12.4.1: distinguish a quiet lower-only floor from a gesture.
 * Runtime thermal stops are identical to P12.4. */
static inline bool s9_p1241_temp_ok(unsigned mode,bool arm,uint64_t age,
    int32_t big,int32_t lit,int32_t gpu)
{
    if(!s9_p124_temp_ok(false,age,big,lit,gpu))return false;
    if(!arm)return true;
    if(mode==1 || mode==2)return big<=68000 && lit<=67000 && gpu<=67000;
    return big<=67000 && lit<=65000 && gpu<=66000;
}
static inline bool s9_p1241_rise_ok(int32_t big,int32_t lit,int32_t gpu,
    int32_t initial_big,int32_t initial_lit,int32_t initial_gpu)
{
    return big-initial_big<3000 && lit-initial_lit<2000 && gpu-initial_gpu<3000;
}
static inline unsigned s9_p124_floor_request(unsigned mode)
{
    return mode==1?949000u:mode==2?832000u:0u;
}
static inline unsigned s9_p124_boost_request(unsigned mode,unsigned original,
    unsigned is_big,unsigned burst,unsigned moving)
{
    if(mode==3){
        /* First trial isolates the initial BIG burst only. */
        return is_big && burst && original>1469000u?1469000u:original;
    }
    if(mode!=4)return original;
    unsigned cap=is_big?(burst?1261000u:(moving?1066000u:1261000u)):1053000u;
    return original<cap?original:cap;
}
#endif
