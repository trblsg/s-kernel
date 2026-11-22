/* S9 Home idle autonomous A/B policy. No Objective-C, shared by device and
 * C11/C++17 UBSan host tests. All failures cancel the gate (fail OPEN).
 * For HI=2 only. HI=1 remains read-only/manual with existing controls. */
#ifndef S9_HI_AUTO_POLICY_H
#define S9_HI_AUTO_POLICY_H
#include <stdint.h>
#include <stdbool.h>
#define HI_AUTO_BOOT_US UINT64_C(180000000)
#define HI_AUTO_INPUT_QUIET_US UINT64_C(8000000)
#define HI_AUTO_CLOCK_TICKS_5S 42u
#define HI_AUTO_SEGMENT_US UINT64_C(20000000)
#define HI_AUTO_CLOCK_GAP_US UINT64_C(3000000)
enum hi_auto_phase {
    HI_AUTO_WAIT=0, HI_AUTO_BASELINE=1, HI_AUTO_CUT1=2,
    HI_AUTO_RECOVER1=3, HI_AUTO_CUT2=4, HI_AUTO_RECOVER2=5,
    HI_AUTO_FINISHED=6, HI_AUTO_ABORTED=7
};
static inline bool
hi_auto_candidate(uint64_t now, uint64_t last_touch, unsigned ticks_5s,
    bool home_ready, bool event_hook_ok)
{
    return home_ready && event_hook_ok && now >= HI_AUTO_BOOT_US &&
        now >= last_touch && now - last_touch >= HI_AUTO_INPUT_QUIET_US &&
        ticks_5s >= HI_AUTO_CLOCK_TICKS_5S;
}
static inline bool
hi_auto_must_abort(uint64_t now, uint64_t initial_touch, uint64_t current_touch,
    uint64_t last_clock_tick, bool home_ready, bool event_hook_ok)
{
    return !home_ready || !event_hook_ok || initial_touch != current_touch ||
        now < last_clock_tick || last_clock_tick == 0 ||
        now - last_clock_tick > HI_AUTO_CLOCK_GAP_US;
}
static inline enum hi_auto_phase
hi_auto_next(enum hi_auto_phase stage)
{
    if (stage >= HI_AUTO_BASELINE && stage < HI_AUTO_RECOVER2)
        return (enum hi_auto_phase)(stage + 1);
    if (stage == HI_AUTO_RECOVER2)
        return HI_AUTO_FINISHED;
    return stage;
}
static inline bool
hi_auto_cut_phase(enum hi_auto_phase stage)
{
    return stage == HI_AUTO_CUT1 || stage == HI_AUTO_CUT2;
}
#endif
