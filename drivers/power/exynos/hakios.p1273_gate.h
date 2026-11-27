#ifndef HAKIOS_P1273_GATE_H
#define HAKIOS_P1273_GATE_H
#include <stdint.h>
#include <stdbool.h>
/* Exact modes only. Qualification is RAM-only, never a boot setting. */
#define P1273_G_USB       1u
#define P1273_G_ABOX      2u
#define P1273_G_BT        4u
#define P1273_G_WIFI      8u
#define P1273_G_TICKER   16u
#define P1273_G_FLOOR    32u
#define P1273_G_C5       64u
#define P1273_G_C6      128u
#define P1273_G_BASE5   31u
/* Keep bits 8/9 reserved for missing/fault status. */
#define P12754_G_832 1024u
#define P12754_G_715 2048u
#define P12754_G_598 4096u
static inline unsigned p12754_floor_prereq(unsigned mode){
    return mode==33?P1273_G_FLOOR:mode==49?(P1273_G_FLOOR|P12754_G_832):mode==65?(P1273_G_FLOOR|P12754_G_832|P12754_G_715):0;
}
static inline unsigned p1273_gate_bit(unsigned mode)
{
    switch(mode){
    case 3:return P1273_G_USB;
    case 5:return P1273_G_ABOX;
    case 257:return P1273_G_BT;
    case 129:return P1273_G_WIFI;
    case 513:return P1273_G_TICKER;
    case 17:return P1273_G_FLOOR;
    case 33:return P12754_G_832;
    case 49:return P12754_G_715;
    case 65:return P12754_G_598;
    case 903:return P1273_G_C5;
    case 919:return P1273_G_C6;
    default:return 0;
    }
}
static inline unsigned p1273_gate_required(unsigned mode)
{
    return mode==903?P1273_G_BASE5:mode==919?(P1273_G_BASE5|P1273_G_FLOOR|P1273_G_C5):0;
}
static inline bool p1273_gate_combined(unsigned mode,unsigned qualified,bool enabled)
{
    unsigned required=p1273_gate_required(mode);
    return enabled && required && (qualified&required)==required;
}
struct p1273_gate_evidence {
    uint64_t usb_quiet,usb_down,usb_up;
    uint64_t abox_stop,abox_resume,abox_wait,audio_feed,audio_capture,audio_dma;
    uint64_t bt_attach,bt_detach,bt_irq,bt_rx,bt_tx,bt_post_rx,bt_post_tx;
    uint64_t wifi_attach,wifi_detach,wifi_handled,wifi_rx,wifi_tx,wifi_post_rx,wifi_post_tx;
    uint64_t ticker_wait,ticker_due,ticker_wake,ticker_cancel;
    uint64_t floor_hits,floor_readback;
    bool floor_restored, fault_free;
};
/* missing = module bits, bit8 = fault. This is evidence of exercise,
 * not a proof of perceptual audio quality or over-air packet integrity. */
static inline unsigned p1273_gate_missing(unsigned mode,const struct p1273_gate_evidence *e)
{
    unsigned need=p1273_gate_bit(mode);
    if(mode==903 || mode==919)need=P1273_G_BASE5|(mode==919?P1273_G_FLOOR:0);
    unsigned bad=e->fault_free?0:256u;
    if((need&P1273_G_USB) && (!e->usb_quiet || (mode==3 && (!e->usb_down || !e->usb_up))))bad|=P1273_G_USB;
    if((need&P1273_G_ABOX) && (!e->abox_stop || e->abox_stop!=e->abox_resume ||
        !e->abox_wait || !e->audio_feed || !e->audio_capture || !e->audio_dma))bad|=P1273_G_ABOX;
    if((need&P1273_G_BT) && (!e->bt_attach || e->bt_attach!=e->bt_detach ||
        !e->bt_irq || !e->bt_rx || !e->bt_tx || !e->bt_post_rx || !e->bt_post_tx))bad|=P1273_G_BT;
    if((need&P1273_G_WIFI) && (!e->wifi_attach || e->wifi_attach!=e->wifi_detach ||
        !e->wifi_handled || !e->wifi_rx || !e->wifi_tx || !e->wifi_post_rx || !e->wifi_post_tx))bad|=P1273_G_WIFI;
    if((need&P1273_G_TICKER) && (!e->ticker_wait || !e->ticker_due || !e->ticker_wake || !e->ticker_cancel))bad|=P1273_G_TICKER;
    unsigned floor=need&(P1273_G_FLOOR|P12754_G_832|P12754_G_715|P12754_G_598);
    if(floor && (!e->floor_hits || !e->floor_readback || !e->floor_restored))bad|=floor;
    return bad;
}
#endif
