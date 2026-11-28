#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/display/hakios.p1278_panel.h"
#include "../../drivers/power/exynos/hakios.p12781_active.h"
static struct p1278_dpu_state dpu(unsigned dual){
    struct p1278_dpu_state d={0};
    d.path=dual?0xb1:0x11;
    d.end=(2959u<<16)|1439;d.scaled_size=(2960u<<16)|1440;
    for(unsigned i=0;i<(dual?2:1);i++){
        d.enc[i][0]=dual?8:1;
        d.enc[i][2]=(128u<<16)|2960;
        d.enc[i][3]=((dual?720u:1440u)<<16)|40;
        d.enc[i][4]=(720u<<16)|720;
    }
    return d;
}
/* Actual S9 log 060150, line 12181. Encoder PPS00 is stored last. */
static struct p1278_dpu_state actual(void){
    struct p1278_dpu_state d={
        .path=0xb1,.connection=0x40,.start=0,.end=0xb8f059f,
        .src_size=0x9090480,.src_offset=0,.img_size=0x9090465,.scaled_size=0xb9005a0,
        .in_con=0x2000000,.dpp_in=0,.base=0xd4000000,.frame=2523,.trigger=0x1011,
        .shadow=0x8000003f,.path0=0x100000,.path1=0x100000
    };
    const uint32_t enc[]={0xe022a,0x30f0,0x30800b90,0x02d00028,0x02d002d0,
        0x020001b4,0x0020046c,0x000a000c,0x027701e9,0x180010f0,
        0x030c2000,0x060b0b33,0x0e1c2a38,0x46546269,0x7077797b,
        0x7d7e0102,0x11000089};
    memcpy(d.enc[0],enc,sizeof(enc));memcpy(d.enc[1],enc,sizeof(enc));
    return d;
}
static void actual_test(void){
    struct p1278_link_state l={.config=0x4887c7f,.resol=0xb9001e0,.cprs=0xa,
        .slice01=0x2d002d0,.slice23=0x2d002d0,.mask=0x8e7b4447,.irq=0x23040000,.fifo=0x4507,.phy=0x40f};
    struct p1278_dpu_state d=actual(),bad=d;
    assert(p1278_link_wqhd(&l)&&p1278_dpu_wqhd(&d));
    assert(p1278_admission(true,&l,&d)==0);
    /* IRQ unmasked and stale DONE must neither reject admission nor count
     * as completion. Passive PHY/FIFO witnesses do not depend on IRQ bits. */
    assert(!(l.mask&(1u<<24))&&(l.irq&(1u<<24)));
    assert(p1278_fifo_drained(l.fifo)&&p1278_phy_drained(l.phy));
    l.mask^=1u<<24;l.irq=0;assert(p1278_admission(true,&l,&d)==0);
    bad.enc[0][0]^=8;assert(p1278_admission(true,&l,&bad)==P78_DSC);
    bad=d;bad.enc[1][0]|=1;assert(!p1278_dpu_wqhd(&bad));
    bad=d;bad.enc[1][0]|=1u<<28;assert(!p1278_dpu_wqhd(&bad));
    bad=d;bad.scaled_size^=1;l.resol^=1;l.fifo=0x407;
    assert(p1278_admission(true,&l,&bad)==(P78_DSC|P78_LINK|P78_FIFO));
    assert(p1278_admission(false,&l,&bad)==(P78_CAPTURE|P78_LINK|P78_FIFO));
    assert(!p1278_phy_drained(0x40e));assert(!p1278_phy_drained(0x41f));assert(!p1278_phy_drained(0x20f));
    assert(!p1278_fifo_drained(0xf00));assert(!p1278_fifo_drained(0x100));
    bad=d;bad.path1^=1;assert(!p1278_dpu_same(&d,&bad));
    bad=d;bad.src_size^=1;assert(!p1278_dpu_same(&d,&bad));
    puts("P1278.1 actual 060150 dual encoder snapshot and refusal bitmask PASS");
}
static void active_test(void){
    assert(p12781_tail_target(1248000,false,true,2,30,60,false,false,false)==1053000);
    assert(p12781_tail_target(1469000,true,true,2,30,60,false,false,false)==1261000);
    assert(p12781_tail_target(949000,false,true,2,30,60,false,false,false)==949000);
    assert(p12781_tail_target(1469000,true,true,2,70,60,false,false,false)==1469000);
    assert(p12781_tail_target(1469000,true,true,2,30,150,false,false,false)==1469000);
    assert(p12781_tail_target(1469000,true,true,2,30,60,true,false,false)==1469000);
    assert(p12781_tail_target(1469000,true,true,2,30,60,false,true,false)==1469000);
    assert(p12781_tail_target(1469000,true,true,2,30,60,false,false,true)==1469000);
    assert(p12781_tail_target(1469000,true,false,2,30,60,false,false,false)==1469000);
    assert(p12781_tail_target(1469000,true,true,4,30,60,false,false,false)==1469000);
    puts("P1278.1 policy2 lower-only touch-tail and load/animation/late bypass PASS");
}
int main(void){
    actual_test();active_test();
    assert(p1278_panel_id_ok(0x910443));assert(p1278_panel_id_ok(0x910444));assert(!p1278_panel_id_ok(0x910442));
    struct p1278_link_state l={.config=0x4887c7f,.resol=(2960u<<16)|480,.cprs=2,.slice01=(720u<<16)|720};
    assert(p1278_link_wqhd(&l));struct p1278_link_state q=l;q.resol^=1;assert(!p1278_link_wqhd(&q));assert(!p1278_link_same(&l,&q));
    q=l;q.config|=1u<<18;assert(!p1278_link_wqhd(&q));
    q=l;q.slice01^=1;assert(!p1278_link_wqhd(&q));
    q=l;q.cprs=1;assert(!p1278_link_wqhd(&q));
    for(unsigned k=0;k<2;k++){
        struct p1278_dpu_state d=dpu(k),e=d;
        assert(p1278_dpu_wqhd(&d));assert(p1278_dpu_same(&d,&e));
        e.end^=1;assert(!p1278_dpu_wqhd(&e));assert(!p1278_dpu_same(&d,&e));
        e=d;e.enc[0][4]^=1;assert(!p1278_dpu_wqhd(&e));
        e=d;e.enc[0][6]^=1;assert(!p1278_dpu_same(&d,&e));
        e=d;e.src_offset=1;assert(!p1278_dpu_wqhd(&e));
        e=d;e.err_i=1;assert(!p1278_dpu_wqhd(&e));
        e=d;e.err_d=1;assert(!p1278_dpu_wqhd(&e));
        e=d;e.frame=99;e.base=0x12340000;assert(p1278_dpu_same(&d,&e));
    }
    assert(sizeof(p1278_wqhd_pps)==128);
    assert(p1278_wqhd_pps[6]*256+p1278_wqhd_pps[7]==2960);
    assert(p1278_wqhd_pps[8]*256+p1278_wqhd_pps[9]==1440);
    assert(p1278_wqhd_pps[10]*256+p1278_wqhd_pps[11]==40);
    assert(p1278_wqhd_pps[12]*256+p1278_wqhd_pps[13]==720);
    puts("P1278 panel identity, WQHD geometry, encoder/DSIM mismatch and PPS tests PASS (pure host test)");
    return 0;
}
