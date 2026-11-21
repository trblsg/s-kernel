/* Portable regression for P6.6 classification: candidate != verified output. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../userland/src/s9_p66_frame_bucket.h"
int main(void){
    struct s9_p66_bucket b;
    struct s9_p66_event e={0};
    s9_p66_reset(&b);
    e.target_id=101;e.targets=1;e.draws=23;e.checked=3;
    e.same=3;e.presented=1;e.end_reason=1;
    s9_p66_add(&b,&e);
    assert(b.frames==1 && b.input_candidate_frames==1 && b.max_candidate_streak==1);
    s9_p66_add(&b,&e);
    assert(b.stable_target==1 && b.candidate_streak==2 && b.max_candidate_streak==2);
    e.changed=1;e.same=2;
    s9_p66_add(&b,&e);
    assert(b.changed_frames==1 && b.input_candidate_frames==2 && !b.candidate_streak);
    e.changed=0;e.same=3;e.skipped=2;
    s9_p66_add(&b,&e);
    assert(b.skipped_frames==1 && b.input_candidate_frames==2);
    e.skipped=0;e.checked=0;e.same=0;
    s9_p66_add(&b,&e);
    assert(b.unchecked_frames==1 && b.input_candidate_frames==2);
    e.checked=3;e.same=3;e.target_id=202;
    s9_p66_add(&b,&e);
    assert(b.switched_target==1 && b.input_candidate_frames==3);
    e.targets=2;
    s9_p66_add(&b,&e);
    assert(b.multi_target==1 && b.unknown_target==1 && b.input_candidate_frames==3);
    e.targets=0;e.target_id=0;e.draws=0;e.presented=0;e.gap_us=2500000u;
    s9_p66_add(&b,&e);
    assert(b.no_target==1 && b.no_draw_frames==1 && b.nonpresented_frames==1);
    assert(b.long_gap_frames==1);
    assert(b.flush_frames==8 && b.frames==8);
    assert(b.single_target==6 && b.multi_target==1 && b.no_target==1);
    s9_p66_reset(&b);
    assert(b.frames==0 && b.input_candidate_frames==0 && b.prev_known==0);
    puts("P6.6 frame metadata classifiers PASS; no framebuffer identity inferred");
    return 0;
}
