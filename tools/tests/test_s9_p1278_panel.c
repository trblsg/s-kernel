#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/display/hakios.p1278_panel.h"
static struct p1278_dpu_state dpu(unsigned dual){
    struct p1278_dpu_state d={0};
    d.path=dual?0xb1:0x11;
    d.end=(2959u<<16)|1439;d.scaled_size=(2960u<<16)|1440;
    for(unsigned i=0;i<(dual?2:1);i++){
        d.enc[i][0]=dual?0:1;
        d.enc[i][2]=(128u<<16)|2960;
        d.enc[i][3]=((dual?720u:1440u)<<16)|40;
        d.enc[i][4]=(720u<<16)|720;
    }
    return d;
}
int main(void){
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
