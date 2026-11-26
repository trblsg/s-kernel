/* P12.7.4 pure policy helpers. All rates are upper limits, never raises. */
#ifndef HAKIOS_P1274_POLICY_H
#define HAKIOS_P1274_POLICY_H
#include <stdint.h>
#include <stdbool.h>
enum { P1274_BASE=1,P1274_LOG2,P1274_FAST,P1274_CAP1469,P1274_CAP1261,
       P1274_CAP1066,P1274_REC2,P1274_REC1,P1274_REC0 };
static inline uint32_t s9_p1274_cap(unsigned p) {
    switch(p){case P1274_CAP1469:return 1469000;case P1274_CAP1261:return 1261000;
    case P1274_CAP1066:return 1066000;default:return 0;}
}
static inline unsigned s9_p1274_cores(unsigned p) {
    return p==P1274_REC2?2:p==P1274_REC1?1:p==P1274_REC0?0:4;
}
static inline bool s9_p1274_temp_valid(int32_t mc) {return mc>=0 && mc<=120000;}
/* Separate ceiling: never clear on a bad sample. Release only after a
 * continuous cool interval, by the governor owner; no host-dependent release. */
static inline uint32_t s9_p1274_fast_cap(unsigned p,int32_t mc,bool rising,uint32_t old) {
    if(!s9_p1274_temp_valid(mc))return old;
    uint32_t cap=0;
    if(mc>=76000)cap=1066000; /* emergency even for observational profiles */
    else if(p>=P1274_FAST){
        if(mc>=73000 || (mc>=68000 && rising))cap=1261000;
        else if(mc>=70000 || (mc>=65000 && rising))cap=1469000;
    }
    return !old?cap:!cap?old:old<cap?old:cap;
}
static inline unsigned s9_p1274_trigger(bool eligible,bool sustained,int32_t mc,bool rising) {
    if(!eligible)return 0;
    return (sustained?1u:0u)|((s9_p1274_temp_valid(mc)&&mc>=70000)?2u:0u)|(rising?4u:0u);
}
#endif
