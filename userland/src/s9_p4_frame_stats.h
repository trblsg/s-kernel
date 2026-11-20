/* SPDX-License-Identifier: MIT
 * P4: allocation-free frame accounting. NO GL calls, no frame skipping,
 * no assumption that repeated inputs imply the final pixels are equal.
 */
#ifndef S9_P4_FRAME_STATS_H
#define S9_P4_FRAME_STATS_H
#include <stdint.h>
#include <string.h>
enum s9_p4_end_reason {
    S9_P4_GL_FLUSH=1, S9_P4_GL_FINISH=2, S9_P4_GL_FINISH_OBJECT=3
};
struct s9_p4_frame_event {
    uint32_t gl_calls, gl_draws, gl_clears, gl_uploads;
    uint32_t checked, same, changed, skipped;
    uint32_t host_presents, in_place, copied, end_reason;
    uint64_t duration_us, gap_us;
};
struct s9_p4_frame_window {
    uint32_t frames, frames_checked, frames_changed, frames_candidate;
    uint32_t frames_unchecked, frames_no_draw, frames_host_present, frames_with_copy;
    uint32_t finish_flush, finish_wait, finish_object;
    uint32_t gap_lt17, gap_lt50, gap_lt120, gap_ge120, gap_unknown;
    uint64_t calls, draws, clears, uploads, checked, same, changed, skipped;
    uint64_t presented, copies, in_place, elapsed_us;
};
static inline void
s9_p4_frame_add(struct s9_p4_frame_window *w, const struct s9_p4_frame_event *e)
{
    w->frames++;
    w->calls+=e->gl_calls;
    w->draws+=e->gl_draws;
    w->clears+=e->gl_clears;
    w->uploads+=e->gl_uploads;
    w->checked+=e->checked;
    w->same+=e->same;
    w->changed+=e->changed;
    w->skipped+=e->skipped;
    w->presented+=e->host_presents;
    w->copies+=e->copied;
    w->in_place+=e->in_place;
    w->elapsed_us+=e->duration_us;
    w->frames_no_draw+=(e->gl_draws==0);
    w->frames_host_present+=(e->host_presents!=0);
    w->frames_with_copy+=(e->copied!=0);
    w->frames_checked+=(e->checked!=0);
    w->frames_unchecked+=(e->checked==0);
    w->frames_changed+=(e->changed!=0);
    /* Shadow-only flag: repeated *checked texture inputs* are a reason
     * to investigate, NEVER proof the frame pixels/animations are same. */
    w->frames_candidate+=(e->checked!=0 && e->changed==0 &&
                          e->gl_uploads==0 && e->same==e->checked);
    w->finish_flush+=(e->end_reason==S9_P4_GL_FLUSH);
    w->finish_wait+=(e->end_reason==S9_P4_GL_FINISH);
    w->finish_object+=(e->end_reason==S9_P4_GL_FINISH_OBJECT);
    if(e->gap_us==0) w->gap_unknown++;
    else if(e->gap_us<17000u) w->gap_lt17++;
    else if(e->gap_us<50000u) w->gap_lt50++;
    else if(e->gap_us<120000u) w->gap_lt120++;
    else w->gap_ge120++;
}
static inline void s9_p4_frame_reset(struct s9_p4_frame_window *w) {
    memset(w,0,sizeof(*w));
}
#endif
