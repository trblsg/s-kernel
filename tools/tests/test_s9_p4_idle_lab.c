/* S9 P4 Idle/Compositor passive arithmetic regression.
 * No Apple frameworks; portable strict C11 and C++17 with UBSan.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/power/exynos/hakios.p4_idle_math.h"
#include "../../userland/src/s9_p4_frame_stats.h"
#include "../../userland/src/s9_p4_thread_delta.h"
static void cpu_idle_tests(void) {
    assert(s9_p4_idle_percent(1000,1000,1000,1,1)==0);
    assert(s9_p4_idle_percent(1000,1250,1000,1,1)==25);
    assert(s9_p4_idle_percent(1000,2000,1000,1,1)==100);
    assert(s9_p4_idle_percent(1000,5000,1000,1,1)==100);
    assert(s9_p4_idle_percent(1000,999,1000,1,1)==S9_P4_NO_SAMPLE);
    assert(s9_p4_idle_percent(0,500,1000,0,1)==S9_P4_NO_SAMPLE);
    assert(s9_p4_idle_percent(0,500,1000,1,0)==S9_P4_NO_SAMPLE);
    assert(s9_p4_idle_percent(0,500,0,1,1)==S9_P4_NO_SAMPLE);
    /* Avoid overflow with large but valid monotonic idle ticks. */
    assert(s9_p4_idle_percent(UINT64_MAX-1000,UINT64_MAX-750,1000,1,1)==25);
    assert(s9_p4_gap_bucket(0,17,50,120)==4);
    assert(s9_p4_gap_bucket(16,17,50,120)==0);
    assert(s9_p4_gap_bucket(17,17,50,120)==1);
    assert(s9_p4_gap_bucket(50,17,50,120)==2);
    assert(s9_p4_gap_bucket(120,17,50,120)==3);
}
static void frame_tests(void) {
    struct s9_p4_frame_window w;
    struct s9_p4_frame_event e;
    memset(&w,0,sizeof(w));
    memset(&e,0,sizeof(e));
    /* A sampled unchanged texture is not sufficient to assert the actual
     * framebuffer is unchanged. The helper produces candidate evidence. */
    e.gl_calls=120;e.gl_draws=23;e.gl_clears=1;
    e.checked=5;e.same=5;e.skipped=40;
    e.host_presents=1;e.in_place=1;e.end_reason=S9_P4_GL_FLUSH;
    e.duration_us=6500;e.gap_us=100000;
    s9_p4_frame_add(&w,&e);
    assert(w.frames==1 && w.frames_candidate==1 && w.frames_checked==1);
    assert(w.gap_lt120==1 && w.finish_flush==1 && w.draws==23);
    assert(w.skipped==40 && w.frames_host_present==1);
    e.changed=1;e.same=4;
    s9_p4_frame_add(&w,&e);
    assert(w.frames_changed==1 && w.frames_candidate==1);
    e.checked=0;e.same=0;e.changed=0;e.gl_uploads=0;
    s9_p4_frame_add(&w,&e);
    assert(w.frames_unchecked==1 && w.frames_candidate==1);
    e.checked=2;e.same=2;e.gl_uploads=1;
    s9_p4_frame_add(&w,&e);
    assert(w.frames_candidate==1);
    e.gl_uploads=0;e.checked=0;e.same=0;e.gl_draws=0;
    e.end_reason=S9_P4_GL_FINISH;
    e.gap_us=0;
    s9_p4_frame_add(&w,&e);
    assert(w.frames_no_draw==1 && w.finish_wait==1 && w.gap_unknown==1);
    e.gap_us=18000;e.end_reason=S9_P4_GL_FINISH_OBJECT;
    s9_p4_frame_add(&w,&e);
    assert(w.gap_lt50==1 && w.finish_object==1);
    e.gap_us=150000;
    s9_p4_frame_add(&w,&e);
    assert(w.gap_ge120==1);
    assert(w.frames==7 && w.calls==7*120);
    s9_p4_frame_reset(&w);
    assert(w.frames==0 && w.draws==0 && w.frames_candidate==0);
}
static void thread_tests(void) {
    assert(s9_p4_delta(123,456)==333);
    assert(s9_p4_delta(456,123)==0);
    assert(!s9_p4_elapsed_valid(1,1));
    assert(s9_p4_elapsed_valid(0,123));
    assert(!s9_p4_elapsed_valid(UINT64_MAX,0));
}
int main(void) {
    cpu_idle_tests();frame_tests();thread_tests();
    puts("S9 P4 idle math, frame evidence and thread counter deltas: PASS");
    return 0;
}
