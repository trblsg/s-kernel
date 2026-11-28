#ifndef HAKIOS_P12781_ACTIVE_H
#define HAKIOS_P12781_ACTIVE_H
#include <stdint.h>
#include <stdbool.h>
/* Only the Scion policy-2 touch tail, not demand/finger/animation floors.
 * Stock cap is always an upper bound; no extra ACPM or scheduler writes. */
static inline uint32_t p12781_tail_target(uint32_t stock,bool big,bool enabled,unsigned policy,
    unsigned load,unsigned sum,bool held,bool moving,bool late){
    if(!enabled || policy!=2 || load>=70 || sum>=150 || held || moving || late)return stock;
    uint32_t cap=big?1261000u:1053000u;
    return stock<cap?stock:cap;
}
#endif
