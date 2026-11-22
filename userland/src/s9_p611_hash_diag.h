/* SPDX-License-Identifier: MIT
 * P6.11: NO new IOSurface reads or hash passes. Counts reasons on the
 * existing surface_changed path; time one of every 16 REAL checks only.
 * IDs tracked in 8 bounded slots; overflow explicitly counted.
 */
#ifndef S9_P611_HASH_DIAG_H
#define S9_P611_HASH_DIAG_H
#include <stdint.h>
#include <string.h>
#define S9_P611_SURFACES 8
struct s9_p611_surface {
    uint32_t id, samples, same, changed;
    uint64_t bytes, us, max_us;
};
struct s9_p611_window {
    uint32_t frames, calls, skip_same_frame, skip_no_base, skip_planar, skip_seldom;
    uint32_t checked, checked_same, checked_changed, samples, sampled_changed;
    uint32_t overflow_samples;
    uint64_t sample_bytes, sample_us, max_sample_us;
    struct s9_p611_surface surfaces[S9_P611_SURFACES];
};
static inline void s9_p611_reset(struct s9_p611_window *w){
    memset(w,0,sizeof(*w));
}
static inline void s9_p611_skip(struct s9_p611_window *w,unsigned reason){
    w->calls++;
    switch(reason){
    case 1:w->skip_same_frame++;break;
    case 2:w->skip_no_base++;break;
    case 3:w->skip_planar++;break;
    case 4:w->skip_seldom++;break;
    default:break;
    }
}
static inline void s9_p611_check(struct s9_p611_window *w,
    uint32_t id,uint64_t bytes,uint64_t time_us,int changed,int sampled)
{
    w->calls++;w->checked++;
    if(changed)w->checked_changed++;else w->checked_same++;
    if(!sampled)return;
    w->samples++;w->sample_bytes+=bytes;w->sample_us+=time_us;
    if(changed)w->sampled_changed++;
    if(time_us>w->max_sample_us)w->max_sample_us=time_us;
    unsigned i;
    for(i=0;i<S9_P611_SURFACES;i++)
        if(w->surfaces[i].id==id || w->surfaces[i].id==0)break;
    if(i==S9_P611_SURFACES){
        w->overflow_samples++;return;
    }
    struct s9_p611_surface *s=&w->surfaces[i];
    if(s->id==0)s->id=id;
    s->samples++;s->bytes+=bytes;s->us+=time_us;
    if(changed)s->changed++;else s->same++;
    if(time_us>s->max_us)s->max_us=time_us;
}
#endif
