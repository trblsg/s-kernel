/* SPDX-License-Identifier: MIT. P6.10/P6.11/P6.12 portable C11 and C++17 tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/power/exynos/s9_p610_gov_diag.h"
#include "../../userland/src/s9_p611_hash_diag.h"
#include "../../userland/src/s9_p612_output_diag.h"
int main(void) {
    struct s9_p610_window g;
    struct s9_p610_event ev;
    s9_p610_clear(&g);memset(&ev,0,sizeof(ev));
    ev.elapsed_ms=40;ev.gap_ms=100;ev.age_ms=36;ev.quiet_ms=36;ev.frame_valid=1;
    ev.new_frame=1;ev.from_anim=1;ev.to_anim=1;
    s9_p610_add(&g,&ev);
    assert(g.windows==1 && g.sticky_windows==1 && g.sticky_ms==40 &&
        g.shadow_static_windows==1 && g.gap_51_150==1 && g.real_anim==1);
    ev.new_frame=0;ev.age_ms=140;ev.quiet_ms=140;
    s9_p610_add(&g,&ev);
    assert(g.windows==2 && g.gap_51_150==1 && g.sticky_ms==80);
    ev.moving=1;ev.gap_ms=30;ev.new_frame=1;
    s9_p610_add(&g,&ev);
    assert(g.gap_le50==1 && g.sticky_windows==2 && g.moving_windows==1);
    ev.moving=0;ev.age_ms=350;ev.quiet_ms=350;ev.from_anim=1;ev.to_anim=0;ev.to_static=1;
    ev.transitioned=1;ev.new_frame=0;
    s9_p610_add(&g,&ev);
    assert(g.transitions==1 && g.real_static==1 && g.age_ge300==1);
    ev.fingers=1;ev.age_ms=10;ev.quiet_ms=10;ev.to_static=0;ev.to_anim=0;
    s9_p610_add(&g,&ev);
    assert(g.finger_windows==1 && g.sticky_windows==2);
    ev.fingers=0;ev.frame_valid=0;ev.from_anim=1;ev.to_anim=1;
    s9_p610_add(&g,&ev);
    assert(g.invalid_frame_windows==1 && g.sticky_windows==2);
    s9_p610_clear(&g);assert(!g.windows);

    struct s9_p611_window h;
    s9_p611_reset(&h);
    s9_p611_skip(&h,1);s9_p611_skip(&h,2);
    s9_p611_skip(&h,3);s9_p611_skip(&h,4);
    s9_p611_check(&h,0,0,0,0,0);
    s9_p611_check(&h,123,4096,20,0,1);
    s9_p611_check(&h,123,4096,15,1,1);
    s9_p611_check(&h,456,8192,70,0,1);
    assert(h.calls==8 && h.checked==4 && h.checked_changed==1);
    assert(h.skip_same_frame==1 && h.skip_no_base==1 &&
        h.skip_planar==1 && h.skip_seldom==1);
    assert(h.samples==3 && h.sample_bytes==16384 && h.sample_us==105 &&
        h.max_sample_us==70 && h.sampled_changed==1);
    assert(h.surfaces[0].id==123 && h.surfaces[0].samples==2 &&
        h.surfaces[0].us==35 && h.surfaces[1].id==456);
    for(unsigned id=1000;id<1008;id++)
        s9_p611_check(&h,id,100,5,0,1);
    assert(h.overflow_samples==2 && h.samples==11);
    s9_p611_reset(&h);assert(!h.samples && !h.surfaces[0].id);

    struct s9_p612_tracker t;
    struct s9_p612_frame f;
    s9_p612_reset(&t);memset(&f,0,sizeof(f));
    f.targets=1;f.id=10;f.draws=23;f.api_calls=1;f.api_zero=1;f.shared=1;
    s9_p612_add(&t,&f);
    assert(t.group.singles==1 && t.group.api_zero==1 && t.group.target_a==10);
    f.id=20;s9_p612_add(&t,&f);
    assert(t.group.switched==1 && t.group.target_b==20);
    f.id=10;s9_p612_add(&t,&f);
    f.id=20;s9_p612_add(&t,&f);
    assert(t.group.alternating==2 && t.group.switched==3);
    s9_p612_clear_window(&t);
    f.id=10;f.api_zero=0;f.api_positive=1;
    s9_p612_add(&t,&f);
    assert(t.group.frames==1 && t.group.alternating==1 &&
        t.group.target_a==10 && t.group.target_b==20);
    f.id=30;s9_p612_add(&t,&f);
    assert(t.group.third_targets==1);
    f.targets=2;s9_p612_add(&t,&f);
    assert(t.group.multis==1);
    f.targets=0;f.id=0;s9_p612_add(&t,&f);
    assert(t.group.unknown==1);
    s9_p612_reset(&t);assert(!t.group.frames && !t.a);
    puts("P6.10/11/12 pure state/hash/output observers PASS");
    return 0;
}
