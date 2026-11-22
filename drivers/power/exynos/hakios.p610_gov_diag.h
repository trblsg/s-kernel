/* SPDX-License-Identifier: MIT
 * P6.10: passive Scion state/cadence evidence, not a governor policy.
 * Portable to C11 and C++17 for sanitizer tests.
 */
#ifndef S9_P610_GOV_DIAG_H
#define S9_P610_GOV_DIAG_H
#include <stdint.h>
#include <string.h>
struct s9_p610_event {
    uint32_t elapsed_ms, gap_ms, age_ms, quiet_ms;
    uint32_t frame_valid, new_frame, moving, fingers, from_anim, to_anim, to_static, transitioned;
};
struct s9_p610_window {
    uint64_t windows, elapsed_ms, sticky_windows, sticky_ms, shadow_static_windows;
    uint64_t moving_windows, real_anim, real_static, finger_windows, transitions;
    uint64_t gap_le50, gap_51_150, gap_151_300, gap_gt300;
    uint64_t age_le100, age_101_299, age_ge300, invalid_frame_windows;
    uint32_t last_gap_ms, last_age_ms, last_quiet_ms, max_age_ms;
};
static inline void s9_p610_clear(struct s9_p610_window *w) { memset(w,0,sizeof(*w)); }
static inline void s9_p610_add(struct s9_p610_window *w, const struct s9_p610_event *e) {
    int sticky=e->frame_valid && e->from_anim && e->to_anim && !e->moving &&
        !e->fingers && e->age_ms<300 && e->quiet_ms<300 && e->gap_ms>50;
    w->windows++;
    w->elapsed_ms+=e->elapsed_ms;
    if(e->moving)w->moving_windows++;
    if(e->fingers)w->finger_windows++;
    if(e->to_anim)w->real_anim++;
    if(e->to_static)w->real_static++;
    if(e->transitioned)w->transitions++;
    if(sticky){
        w->sticky_windows++;
        w->sticky_ms+=e->elapsed_ms;
        w->shadow_static_windows++; /* hypothetical reclassification, NOT applied */
    }
    if(e->new_frame){
        if(e->gap_ms<=50)w->gap_le50++;
        else if(e->gap_ms<=150)w->gap_51_150++;
        else if(e->gap_ms<=300)w->gap_151_300++;
        else w->gap_gt300++;
    }
    if(!e->frame_valid)w->invalid_frame_windows++;
    else if(e->age_ms<=100)w->age_le100++;
    else if(e->age_ms<300)w->age_101_299++;
    else w->age_ge300++;
    w->last_gap_ms=e->gap_ms;
    w->last_age_ms=e->frame_valid?e->age_ms:0;
    w->last_quiet_ms=e->quiet_ms;
    if(e->age_ms>w->max_age_ms)w->max_age_ms=e->age_ms;
}
#endif
