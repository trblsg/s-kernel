/* P12.2 pure-C source policy, histogram and explicit fallbacks. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../drivers/gpu/mali/hakios.p122_diag.h"
#include "../../userland/src/s9_p122_frame_diag.h"
int main(void) {
    assert(s9_p122_mode_valid(0) && s9_p122_mode_valid(1) && s9_p122_mode_valid(2));
    assert(!s9_p122_mode_valid(-1) && !s9_p122_mode_valid(3));
    assert(s9_p122_bucket_us(0)==0 && s9_p122_bucket_us(250)==0);
    assert(s9_p122_bucket_us(251)==1 && s9_p122_bucket_us(500)==1);
    assert(s9_p122_bucket_us(501)==2 && s9_p122_bucket_us(16000)==6);
    assert(s9_p122_bucket_us(16001)==7);
    assert(s9_p122_frame_bin(16667)==3 && s9_p122_frame_bin(16668)==4);
    assert(s9_p122_frame_bin(50001)==7);
    struct s9_p122_frame_window w={0};
    for(unsigned i=0;i<60;i++){
        s9_p122_frame_add(&w,i==59?48000:6000,20,3,300,
            (i%16)==0,4000);
    }
    assert(w.frames==60 && w.calls==18000 && w.draws==1200);
    assert(w.uploads==180 && w.cpu_samples==4 && w.sampled_cpu_us==16000);
    assert(w.max_us==48000 && w.bins[1]==59 && w.bins[6]==1);
    assert(s9_p122_frame_percentile_bin(&w,50)==1);
    assert(s9_p122_frame_percentile_bin(&w,95)==1);
    assert(s9_p122_frame_percentile_bin(&w,99)==6);
    s9_p122_frame_reset(&w);
    assert(w.frames==0 && w.wall_us==0 && w.cpu_samples==0);
    assert(s9_p122_frame_percentile_bin(&w,95)==0);
    puts("P12.2 ROOT CAUSE PASS: mode range, GPU q/run latency bins, frame p50/p95/p99 and thread CPU sample accounting");
    return 0;
}
