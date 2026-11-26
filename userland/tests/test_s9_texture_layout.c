/* Portable S9 layout arithmetic regression tests; no Apple headers needed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../src/metalvk/s9_texture_layout.h"

static void linear(void) {
    s9_texture_layout p={0};
    assert(s9_tex_plan(80,194,100,784,4096,&p));
    assert(p.row_bytes==784 && p.block_rows==100 && p.size==81920);
    assert(p.alignment==4096 && p.block_width==1);
    uint64_t offset=0,row=0,rows=0;
    assert(s9_tex_region(&p,3,4,10,2,&offset,&row,&rows));
    assert(offset==4*784+3*4 && row==40 && rows==2);
    assert(!s9_tex_region(&p,195,0,10,2,0,0,0));
    assert(!s9_tex_region(&p,194,0,1,1,0,0,0)); /* row padding is not pixels */
    assert(!s9_tex_plan(80,194,100,0,4096,&p)); /* no guessed linear pitch */
    assert(s9_tex_linear_span(8,784,40,2,824));
    assert(!s9_tex_linear_span(8,784,40,2,823));
    assert(!s9_tex_linear_span(UINT64_MAX,784,40,2,0));
    assert(!s9_tex_region(&p,190,95,8,10,0,0,0));
    assert(!s9_tex_plan(80,194,100,768,4096,&p)); /* pitch too short */
    assert(!s9_tex_plan(80,194,100,0,3000,&p)); /* non-power-of-two page */
}
static void blocks(void) {
    s9_texture_layout p={0};uint64_t o=0,b=0,n=0;
    assert(s9_tex_plan(186,5,9,0,4096,&p)); /* ASTC 4x4: 2x3 blocks */
    assert(p.block_width==4 && p.block_height==4);
    assert(p.bytes_per_block==16 && p.row_bytes==32 && p.block_rows==3);
    assert(p.size==4096);
    assert(s9_tex_region(&p,4,8,1,1,&o,&b,&n));
    assert(o==80 && b==16 && n==1);
    assert(!s9_tex_region(&p,4,4,1,1,0,0,0)); /* partial interior block */
    assert(!s9_tex_region(&p,4,8,2,1,0,0,0)); /* padded edge */
    assert(!s9_tex_region(&p,2,4,1,1,0,0,0)); /* non-block origin */
    assert(!s9_tex_plan(191,8,8,0,4096,&p)); /* invalid ASTC family hole */
    assert(s9_tex_plan(180,8,8,0,4096,&p)); /* ETC2 4x4, 8 bytes */
    assert(p.row_bytes==16 && p.block_rows==2);
    assert(s9_tex_plan(178,8,8,0,4096,&p)); /* EAC 16-byte blocks */
    assert(p.row_bytes==32);
}
static void failures(void) {
    s9_texture_layout p={0};
    assert(!s9_tex_plan(252,8,8,0,4096,&p)); /* depth: unknown */
    assert(!s9_tex_plan(80,0,8,0,4096,&p));
    assert(!s9_tex_plan(80,1,UINT64_MAX,0,4096,&p));
    assert(!s9_tex_plan(80,UINT64_MAX,2,0,4096,&p));
    assert(!s9_tex_plan(80,8,8,0,4096,NULL));
    assert(!s9_tex_plan(80,8,8,0,0,&p));
}
int main(void) {
    linear();blocks();failures();
    puts("s9_texture_layout: linear, block, region, unknown and overflow tests PASS");
    return 0;
}
