/* Portable strict C11 / C++17 tests for P1 exact switch and G1 telemetry.
 * No Apple SDK required. Checks category arithmetic, top-slot selection, reset
 * and bounded invalid-slot rejection; these do NOT prove Objective-C runtime.
 */
#include "../src/metalvk/s9_bootarg_flag.h"
#include "../src/s9_gl_cost.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
int main(void) {
    assert(s9_has_boot_flag("p1_userdict=1 glcost=1 mali_gov_frame=455","p1_userdict=1"));
    assert(s9_has_boot_flag(" metal_layout=0\tglcost=1\n", "glcost=1"));
    assert(!s9_has_boot_flag("no_p1_userdict=1", "p1_userdict=1"));
    assert(!s9_has_boot_flag("p1_userdict=10", "p1_userdict=1"));
    assert(!s9_has_boot_flag("p1_userdict=0", "p1_userdict=1"));
    assert(!s9_has_boot_flag("", "p1_userdict=1"));
    assert(!s9_has_boot_flag(NULL, "p1_userdict=1"));
    assert(!s9_has_boot_flag("p1_userdict=1", ""));
    assert(s9_gl_cost_group_of(0x210)==S9_GL_DRAW);
    assert(s9_gl_cost_group_of(0x30)==S9_GL_TEXTURE);
    assert(s9_gl_cost_group_of(0x1368)==S9_GL_UNIFORM);
    assert(s9_gl_cost_group_of(0x12c8)==S9_GL_SHADER);
    assert(s9_gl_cost_group_of(0x248)==S9_GL_STATE);
    assert(s9_gl_cost_group_of(0x1000)==S9_GL_VERTEX);
    assert(s9_gl_cost_group_of(0x1508)==S9_GL_TARGET);
    assert(s9_gl_cost_group_of(0x338)==S9_GL_OTHER);
    struct s9_gl_cost s;
    s9_gl_cost_reset(&s);
    assert(!s9_gl_cost_add(&s,3,12));  /* unaligned slot */
    assert(!s9_gl_cost_add(&s,0x440u*8u,12)); /* out of bounds */
    assert(s9_gl_cost_add(&s,0x30,700));
    assert(s9_gl_cost_add(&s,0x30,300));
    assert(s9_gl_cost_add(&s,0x210,1500));
    assert(s9_gl_cost_add(&s,0x1368,350));
    assert(s9_gl_cost_add(&s,0x12c8,50));
    uint64_t totals[S9_GL_GROUPS];
    uint32_t counts[S9_GL_GROUPS];
    unsigned top[3];
    s9_gl_cost_summary(&s,totals,counts,top);
    assert(totals[S9_GL_TEXTURE]==1000 && counts[S9_GL_TEXTURE]==2);
    assert(totals[S9_GL_DRAW]==1500 && counts[S9_GL_DRAW]==1);
    assert(totals[S9_GL_UNIFORM]==350 && counts[S9_GL_UNIFORM]==1);
    assert(top[0]==0x210/8 && top[1]==0x30/8 && top[2]==0x1368/8);
    assert(s.count[0x30/8]==2);
    s9_gl_cost_reset(&s);
    s9_gl_cost_summary(&s,totals,counts,top);
    assert(top[0]==UINT_MAX && counts[S9_GL_TEXTURE]==0);
    puts("s9_p1_g1: bootarg exactness, 8 GL groups, 3-slot ranking, bounds, reset PASS");
    return 0;
}
