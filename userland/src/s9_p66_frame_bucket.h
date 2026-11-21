/* SPDX-License-Identifier: MIT
 * S9 P6.6: read-only CA/EAGL frame evidence. This is only a classifier for
 * sampled IOSurface inputs and output target identity, NEVER pixel identity.
 * No GPU readback, CRC, draw suppression, framebuffer or sync writes.
 */
#ifndef S9_P66_FRAME_BUCKET_H
#define S9_P66_FRAME_BUCKET_H
#include <stdint.h>
#include <string.h>
struct s9_p66_event {
    uint32_t target_id, targets, draws, clears, uploads;
    uint32_t checked, same, changed, skipped;
    uint32_t in_place, copied, presented, end_reason;
    uint64_t gap_us;
};
struct s9_p66_bucket {
    uint32_t frames, single_target, multi_target, no_target;
    uint32_t stable_target, switched_target, unknown_target;
    uint32_t checked_frames, unchecked_frames, skipped_frames, changed_frames;
    uint32_t input_candidate_frames, candidate_streak, max_candidate_streak;
    uint32_t no_draw_frames, presented_frames, nonpresented_frames;
    uint32_t flush_frames, other_end_frames, long_gap_frames;
    uint64_t draws, clears, uploads, checked, same, changed, skipped;
    uint32_t prev_target, prev_known;
};
static inline void s9_p66_reset(struct s9_p66_bucket *b) {
    memset(b, 0, sizeof(*b));
}
static inline void s9_p66_add(struct s9_p66_bucket *b,
    const struct s9_p66_event *e)
{
    int known = e->targets==1 && e->target_id!=0;
    int candidate = known && e->draws>0 && e->checked>0 &&
        e->changed==0 && e->skipped==0 && e->uploads==0;
    b->frames++;
    b->draws += e->draws;
    b->clears += e->clears;
    b->uploads += e->uploads;
    b->checked += e->checked;
    b->same += e->same;
    b->changed += e->changed;
    b->skipped += e->skipped;
    if(!e->draws)b->no_draw_frames++;
    if(e->checked)b->checked_frames++;else b->unchecked_frames++;
    if(e->skipped)b->skipped_frames++;
    if(e->changed)b->changed_frames++;
    if(e->presented)b->presented_frames++;else b->nonpresented_frames++;
    if(e->end_reason==1)b->flush_frames++;else b->other_end_frames++;
    if(e->gap_us>=2000000ull)b->long_gap_frames++;
    if(e->targets>1)b->multi_target++;
    else if(!e->targets)b->no_target++;
    if(known){
        b->single_target++;
        if(b->prev_known){
            if(b->prev_target==e->target_id)b->stable_target++;
            else b->switched_target++;
        }
        b->prev_target=e->target_id;
        b->prev_known=1;
    } else {
        b->unknown_target++;
        b->prev_known=0;
    }
    if(candidate){
        b->input_candidate_frames++;
        b->candidate_streak++;
        if(b->candidate_streak>b->max_candidate_streak)
            b->max_candidate_streak=b->candidate_streak;
    }else b->candidate_streak=0;
}
#endif
