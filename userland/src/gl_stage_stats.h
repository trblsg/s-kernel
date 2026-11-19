/* SPDX-License-Identifier: MIT
 * S9/Galaxy S9: allocation-free per-frame OpenGL ES stage statistics.
 * These are host CPU wall-clock intervals, NOT GPU execution time.
 * Pure C, so boundary arithmetic can be tested without the iOS SDK.
 */
#ifndef S9_GL_STAGE_STATS_H
#define S9_GL_STAGE_STATS_H
#include <stdint.h>
#include <string.h>

struct s9_gl_stage_stats {
    uint64_t draw_us, present_us, sync_us, copy_us, flush_us, total_us;
    uint64_t max_us;
    uint32_t n, slow16, slow33, presented;
    uint32_t bins[8];  /* <=5/8/12/16.667/20/33.333/50ms, >50ms */
};
static inline unsigned s9_gl_stage_bucket(uint64_t us) {
    static const uint32_t upper[7] = {5000,8000,12000,16667,20000,33333,50000};
    for (unsigned i=0;i<7;++i)
        if (us<=upper[i])return i;
    return 7;
}
static inline void s9_gl_stage_add(struct s9_gl_stage_stats *s,
                                   uint64_t draw, uint64_t present, uint64_t sync,
                                   uint64_t copy, uint64_t flush, uint64_t total,
                                   unsigned shown) {
    ++s->n;
    s->draw_us+=draw;
    s->present_us+=present;
    s->sync_us+=sync;
    s->copy_us+=copy;
    s->flush_us+=flush;
    s->total_us+=total;
    if(total>s->max_us)s->max_us=total;
    s->slow16+=(total>16667);
    s->slow33+=(total>33333);
    s->presented+=shown;
    ++s->bins[s9_gl_stage_bucket(total)];
}
static inline unsigned s9_gl_stage_p95_bucket(const struct s9_gl_stage_stats *s) {
    if (!s->n)return 0;
    uint32_t target=(s->n*95+99)/100,acc=0;
    for(unsigned i=0;i<8;++i){
        acc+=s->bins[i];
        if(acc>=target)return i;
    }
    return 7;
}
static inline void s9_gl_stage_reset(struct s9_gl_stage_stats *s) {
    memset(s,0,sizeof(*s));
}
#endif
