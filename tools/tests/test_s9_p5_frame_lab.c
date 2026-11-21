/* S9 P5 frame/wake evidence portable tests (strict C11 and C++17).
 * These counters must never turn into frame omission decisions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../../userland/src/s9_p5_frame_lab.h"

static void idle_10fps(void)
{
    struct s9_p5_frame_group s;
    struct s9_p5_frame_event e;
    s9_p5_frame_reset(&s);
    memset(&e,0,sizeof(e));
    for(unsigned i=0;i<60;i++){
        e.draws=23;e.clears=1;e.uploads=0;
        e.large_target_draws=16;e.other_draws=7;
        e.checked=i%8==0?1:0;e.same=e.checked;
        e.changed=0;e.skipped=11;
        e.frame_end=1;e.duration_us=6600;e.gap_us=i?100000:0;
        s9_p5_frame_add(&s,&e,i+1);
    }
    assert(s.frames==60);
    assert(s.draws==1380 && s.large_target_draws==960);
    assert(s.checked==8 && s.same==8 && s.changed==0);
    assert(s.skipped==660 && s.unchecked_frames==52);
    assert(s.fast_17ms==0 && s.fast_33ms==0 && s.slow_120ms==0);
    assert(s.gl_flush==60 && s.gl_finish==0);
    assert(s.unknown_gap==1 && s.min_gap_us==100000 && s.max_gap_us==100000);
    assert(s.elapsed_us==396000);
    s9_p5_frame_reset(&s);
    assert(s.frames==0 && s.draws==0 && s.unknown_gap==0);
}

static void wake_60fps(void)
{
    struct s9_p5_frame_group s;
    struct s9_p5_frame_event e;
    s9_p5_frame_reset(&s);
    memset(&e,0,sizeof(e));
    for(unsigned i=0;i<60;i++){
        e.draws=96;e.clears=1;e.uploads=i==11?66:0;
        e.large_target_draws=65;e.other_draws=31;
        e.checked=4;e.same=i==11?2:4;
        e.changed=i==11?2:0;e.skipped=3;
        e.duration_us=15000;e.gap_us=16000;
        e.frame_end=i==20?2:3;
        s9_p5_frame_add(&s,&e,1000+i);
    }
    assert(s.frames==60 && s.draws==5760);
    assert(s.fast_17ms==60 && s.fast_33ms==60);
    assert(s.changed==2 && s.changed_frames==1);
    assert(s.uploads==66 && s.upload_frames==1);
    assert(s.gl_finish==1 && s.gl_finish_object==59);
    assert(s.max_uploads==66 && s.max_draws==96 && s.max_changed==2);
    assert(s.min_gap_us==16000 && s.max_gap_us==16000);
    assert(s.large_target_frames==60 && s.large_target_draws==3900);
}

static void quiet_and_edge(void)
{
    struct s9_p5_frame_group s;
    struct s9_p5_frame_event e;
    s9_p5_frame_reset(&s);
    memset(&e,0,sizeof(e));
    e.draws=1;e.gap_us=2000000;e.frame_end=1;
    s9_p5_frame_add(&s,&e,100);
    assert(s.slow_120ms==1 && s.max_gap_us==2000000);
    e.gap_us=17000;
    s9_p5_frame_add(&s,&e,101);
    assert(s.fast_17ms==0 && s.fast_33ms==1);
    e.gap_us=33000;
    s9_p5_frame_add(&s,&e,102);
    assert(s.fast_33ms==1);
    e.gap_us=120000;
    s9_p5_frame_add(&s,&e,103);
    assert(s.slow_120ms==2);
    e.gap_us=0;
    s9_p5_frame_add(&s,&e,104);
    assert(s.unknown_gap==1);
}
int main(void){
    idle_10fps();
    wake_60fps();
    quiet_and_edge();
    puts("S9 P5 GL 10fps, 60fps wake, quiet gap and evidence bounds: PASS");
    return 0;
}
