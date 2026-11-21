/* SPDX-License-Identifier: MIT
 * S9 P5: bounded, allocation-free display-cadence and wake-burst counters.
 * A frame end GL call is NOT the CoreAnimation timer/vsync origin;
 * a large target is NOT proof that the physical display was updated;
 * repeated checked inputs are NOT proof of identical output pixels.
 */
#ifndef S9_P5_FRAME_LAB_H
#define S9_P5_FRAME_LAB_H
#include <stdint.h>
#include <string.h>
struct s9_p5_frame_event {
    uint32_t draws,clears,uploads,large_target_draws,other_draws;
    uint32_t checked,same,changed,skipped,frame_end;
    uint64_t duration_us,gap_us;
};
struct s9_p5_frame_group {
    uint32_t frames,fast_17ms,fast_33ms,slow_120ms,unknown_gap;
    uint32_t large_target_frames,large_target_draws,other_draws;
    uint32_t changed_frames,unchecked_frames,upload_frames;
    uint32_t gl_flush,gl_finish,gl_finish_object;
    uint32_t max_draws,max_uploads,max_changed,max_skipped;
    uint32_t max_draw_frame,fastest_frame;
    uint32_t top_draws,top_uploads,top_changed,top_target_draws;
    uint64_t elapsed_us,sum_gap_us,min_gap_us,max_gap_us;
    uint64_t draws,clears,uploads,checked,same,changed,skipped;
};
static inline void
s9_p5_frame_add(struct s9_p5_frame_group *g,const struct s9_p5_frame_event *e,
    uint32_t frame)
{
    g->frames++;
    g->draws+=e->draws;g->clears+=e->clears;g->uploads+=e->uploads;
    g->large_target_draws+=e->large_target_draws;g->other_draws+=e->other_draws;
    g->checked+=e->checked;g->same+=e->same;g->changed+=e->changed;
    g->skipped+=e->skipped;g->elapsed_us+=e->duration_us;
    g->large_target_frames+=e->large_target_draws!=0;
    g->changed_frames+=e->changed!=0;g->unchecked_frames+=e->checked==0;
    g->upload_frames+=e->uploads!=0;
    g->gl_flush+=e->frame_end==1;g->gl_finish+=e->frame_end==2;
    g->gl_finish_object+=e->frame_end==3;
    if(e->gap_us==0)g->unknown_gap++;
    else {
        g->sum_gap_us+=e->gap_us;
        if(!g->min_gap_us || e->gap_us<g->min_gap_us){
            g->min_gap_us=e->gap_us;g->fastest_frame=frame;
        }
        if(e->gap_us>g->max_gap_us)g->max_gap_us=e->gap_us;
        g->fast_17ms+=e->gap_us<17000;
        g->fast_33ms+=e->gap_us<33000;
        g->slow_120ms+=e->gap_us>=120000;
    }
    if(e->draws>g->max_draws){
        g->max_draws=e->draws;g->max_draw_frame=frame;
        g->top_draws=e->draws;g->top_uploads=e->uploads;
        g->top_changed=e->changed;g->top_target_draws=e->large_target_draws;
    }
    if(e->uploads>g->max_uploads)g->max_uploads=e->uploads;
    if(e->changed>g->max_changed)g->max_changed=e->changed;
    if(e->skipped>g->max_skipped)g->max_skipped=e->skipped;
}
static inline void
s9_p5_frame_reset(struct s9_p5_frame_group *g)
{
    memset(g,0,sizeof(*g));
}
#endif
