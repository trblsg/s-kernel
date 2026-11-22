/* SPDX-License-Identifier: MIT
 * P6.12: EAGL output target/API observations. Kernel fps counters
 * are correlated OFFLINE, not falsely inferred from hakios_gl_present().
 */
#ifndef S9_P612_OUTPUT_DIAG_H
#define S9_P612_OUTPUT_DIAG_H
#include <stdint.h>
#include <string.h>
struct s9_p612_frame {
    uint32_t id, targets, draws, mirror, shared, copy;
    uint32_t api_calls, api_positive, api_zero, api_negative;
};
struct s9_p612_window {
    uint32_t frames, singles, multis, unknown, switched, same, alternating;
    uint32_t mirror_frames, shared_frames, copy_frames;
    uint32_t api_calls, api_positive, api_zero, api_negative;
    uint32_t draws, target_a, target_b, third_targets;
};
struct s9_p612_tracker {
    struct s9_p612_window group;
    uint32_t previous, before_previous, a, b;
};
static inline void s9_p612_reset(struct s9_p612_tracker *t){
    memset(t,0,sizeof(*t));
}
static inline void s9_p612_clear_window(struct s9_p612_tracker *t){
    memset(&t->group,0,sizeof(t->group));
}
static inline void s9_p612_add(struct s9_p612_tracker *t, const struct s9_p612_frame *f){
    struct s9_p612_window *w=&t->group;
    w->frames++;w->draws+=f->draws;
    if(f->mirror)w->mirror_frames++;
    if(f->shared)w->shared_frames++;
    if(f->copy)w->copy_frames++;
    w->api_calls+=f->api_calls;w->api_positive+=f->api_positive;
    w->api_zero+=f->api_zero;w->api_negative+=f->api_negative;
    if(f->targets>1){w->multis++;t->previous=t->before_previous=0;return;}
    if(!f->targets || !f->id){w->unknown++;t->previous=t->before_previous=0;return;}
    w->singles++;
    if(t->previous){
        if(t->previous==f->id)w->same++;
        else w->switched++;
        if(t->before_previous==f->id && t->previous!=f->id)
            w->alternating++;
    }
    t->before_previous=t->previous;
    t->previous=f->id;
    if(!t->a)t->a=f->id;
    else if(t->a!=f->id && !t->b)t->b=f->id;
    else if(t->a!=f->id && t->b!=f->id)w->third_targets++;
    w->target_a=t->a;w->target_b=t->b;
}
#endif
