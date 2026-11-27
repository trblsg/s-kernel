#include "../../drivers/power/exynos/hakios.p1273_gate.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(p127543_floor_wake(4,100,200,199,20));
    assert(p127543_floor_wake(7,100,200,205,20));
    assert(p127543_floor_wake(7,100,205,200,20));
    assert(!p127543_floor_wake(7,100,200,99,20));
    assert(!p127543_floor_wake(7,100,99,200,20));
    assert(!p127543_floor_wake(7,100,200,221,20));
    assert(!p127543_floor_wake(0,100,200,200,20));
    assert(!p127543_floor_wake(2,100,200,200,20));
    assert(!p127543_floor_wake(7,0,200,200,20));
    const unsigned modes[]={3,5,257,129,513,17,903,919};
    for(unsigned m=0;m<1024;m++){
        unsigned bit=0;
        for(unsigned i=0;i<8;i++)if(m==modes[i])bit=1u<<i;
        if(m==33)bit=P12754_G_832;
        if(m==49)bit=P12754_G_715;
        if(m==65)bit=P12754_G_598;
        assert(p1273_gate_bit(m)==bit);
        for(unsigned q=0;q<256;q++){
            assert(!p1273_gate_combined(m,q,false));
            assert(p1273_gate_combined(m,q,true)==
                ((m==903 && (q&31)==31)||(m==919 && (q&127)==127)));
        }
    }
    struct p1273_gate_evidence e={0};
    assert(p1273_gate_missing(903,&e)==(31|256));
    assert(p1273_gate_missing(919,&e)==(63|256));
    e=(struct p1273_gate_evidence){
        .usb_quiet=1,.usb_down=1,.usb_up=1,
        .abox_stop=1,.abox_resume=1,.abox_wait=1,.audio_feed=1,.audio_capture=1,.audio_dma=1,
        .bt_attach=1,.bt_detach=1,.bt_irq=1,.bt_rx=1,.bt_tx=1,.bt_post_rx=1,.bt_post_tx=1,
        .wifi_attach=1,.wifi_detach=1,.wifi_handled=1,.wifi_rx=1,.wifi_tx=1,.wifi_post_rx=1,.wifi_post_tx=1,
        .ticker_wait=1,.ticker_due=1,.ticker_wake=1,.ticker_cancel=1,
        .floor_hits=1,.floor_readback=1,.floor_restored=true,.fault_free=true
    };
    for(unsigned i=0;i<8;i++)assert(!p1273_gate_missing(modes[i],&e));
    assert(p12754_floor_prereq(33)==P1273_G_FLOOR);
    assert(p12754_floor_prereq(49)==(P1273_G_FLOOR|P12754_G_832));
    assert(p12754_floor_prereq(65)==(P1273_G_FLOOR|P12754_G_832|P12754_G_715));
    for(unsigned m=33;m<=65;m+=16)assert(!p1273_gate_missing(m,&e));
    e.floor_readback=0;assert(p1273_gate_missing(65,&e)==P12754_G_598);e.floor_readback=1;
    e.usb_up=0;assert(p1273_gate_missing(3,&e)==1);
    e.abox_resume=0;e.bt_post_rx=0;e.wifi_handled=0;e.ticker_cancel=0;e.floor_restored=false;
    assert(p1273_gate_missing(919,&e)==62);
    e.fault_free=false;assert(p1273_gate_missing(919,&e)==(62|256));
    puts("P1273 exact masks and functional evidence PASS (portable only)");
    return 0;
}
