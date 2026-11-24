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
    /* P12.3: no fixed-phase CPU sampling; each residue represented
     * exactly once over 256 frames. No claim of unbiased random sampling. */
    unsigned residues[16]={0};
    for(unsigned i=0;i<256;i++)if(s9_p123_sample_frame(i))residues[i%16]++;
    for(unsigned i=0;i<16;i++)assert(residues[i]==1);
    assert(!s9_p123_trial_active(100,0));
    assert(s9_p123_trial_active(100,30100));
    assert(s9_p123_trial_active(30099,30100));
    assert(!s9_p123_trial_active(30100,30100));
    assert(!s9_p123_trial_active(30101,30100));
    assert(s9_p123_frame_streak(0,100)==1);
    assert(s9_p123_frame_streak(6,16)==0);
    assert(s9_p123_frame_streak(6,151)==0);
    assert(s9_p123_frame_streak(32,100)==32);
    assert(s9_p123_floor_eligible(1,1,1,1,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(0,1,1,1,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,0,1,1,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,-1,1,1,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,0,1,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,0,68000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,72000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,-273000,6,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,68000,5,100,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,68000,6,16,99,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,68000,6,100,151,2));
    assert(!s9_p123_floor_eligible(1,1,1,1,68000,6,100,99,9));
    puts("P12.2/P12.3 ROOT CAUSE PASS: histograms, rotating CPU sample and fail-open adaptive floor gates");
    return 0;
}
