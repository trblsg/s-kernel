/* SPDX-License-Identifier: MIT
 * P12.2 passive, bounded per-frame CPU-wall / sampled thread-CPU histograms.
 * A GL frame's elapsed time is NOT GPU execution time. No calls to GLES.
 */
#ifndef S9_P122_FRAME_DIAG_H
#define S9_P122_FRAME_DIAG_H
#include <stdint.h>
#include <string.h>
struct s9_p122_frame_window {
    uint64_t wall_us, max_us, sampled_wall_us, sampled_cpu_us;
    uint64_t draws, uploads, calls;
    uint32_t frames, cpu_samples;
    uint32_t bins[8]; /* <=5,8,12,16.667,20,33.333,50 ms, >50 */
};
static inline unsigned s9_p122_frame_bin(uint64_t us) {
    static const uint32_t bounds[7]={5000,8000,12000,16667,20000,33333,50000};
    for(unsigned i=0;i<7;++i) if(us<=bounds[i])return i;
    return 7;
}
static inline void s9_p122_frame_add(struct s9_p122_frame_window *w,
    uint64_t wall_us,unsigned draws,unsigned uploads,unsigned calls,
    int cpu_valid,uint64_t cpu_us) {
    if(!w)return;
    w->frames++;
    w->wall_us+=wall_us;
    if(wall_us>w->max_us)w->max_us=wall_us;
    w->draws+=draws; w->uploads+=uploads;w->calls+=calls;
    w->bins[s9_p122_frame_bin(wall_us)]++;
    if(cpu_valid){
        w->cpu_samples++;
        w->sampled_wall_us+=wall_us;
        w->sampled_cpu_us+=cpu_us;
    }
}
static inline unsigned s9_p122_frame_percentile_bin(
    const struct s9_p122_frame_window *w,unsigned percentile) {
    if(!w || !w->frames || percentile<1 || percentile>100)return 0;
    uint32_t target=(uint32_t)(((uint64_t)w->frames*percentile+99)/100),total=0;
    for(unsigned i=0;i<8;i++){total+=w->bins[i];if(total>=target)return i;}
    return 7;
}
static inline void s9_p122_frame_reset(struct s9_p122_frame_window *w) {
    if(w)memset(w,0,sizeof(*w));
}
#endif
