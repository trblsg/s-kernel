/* Strict C11/C++17 regression for the S9 per-frame GLES probe.
 * Tests arithmetic at frame-boundaries and that reset never carries samples
 * across A/B windows; no Apple headers required.
 */
#include "../src/gl_stage_stats.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
    struct s9_gl_stage_stats s;
    s9_gl_stage_reset(&s);
    assert(s9_gl_stage_bucket(5000)==0);
    assert(s9_gl_stage_bucket(5001)==1);
    assert(s9_gl_stage_bucket(8000)==1);
    assert(s9_gl_stage_bucket(16667)==3);
    assert(s9_gl_stage_bucket(16668)==4);
    assert(s9_gl_stage_bucket(33333)==5);
    assert(s9_gl_stage_bucket(50001)==7);
    for(unsigned i=0;i<60;i++){
        uint64_t elapsed=i==59?34000:12000;
        s9_gl_stage_add(&s,9000,1000,1500,500,300,elapsed,1);
    }
    assert(s.n==60);
    assert(s.draw_us==540000);
    assert(s.present_us==60000);
    assert(s.sync_us==90000);
    assert(s.total_us==59*12000+34000);
    assert(s.slow16==1 && s.slow33==1);
    assert(s.max_us==34000);
    assert(s.presented==60);
    assert(s.bins[2]==59 && s.bins[6]==1);
    assert(s9_gl_stage_p95_bucket(&s)==2);
    s9_gl_stage_reset(&s);
    assert(s.n==0 && s.total_us==0 && s.bins[2]==0 &&
           s.slow16==0 && s.max_us==0);
    s9_gl_stage_add(&s,2000,500,500,0,100,3000,0);
    assert(s.n==1 && s9_gl_stage_p95_bucket(&s)==0);
    puts("gl_stage_stats: 60-frame buckets, p95, slow counters, reset: PASS");
    return 0;
}
