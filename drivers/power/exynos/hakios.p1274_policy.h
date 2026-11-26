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
    /* BASE and LOG2 share the same proactive safety envelope. FAST and
     * the intervention profiles use earlier thresholds; p==0 is the
     * common guard after the measurement lease ends. */
    if(mc>=76000 || (p>=P1274_FAST && mc>=73000))cap=1066000;
    else if(mc>=73000 || (mc>=68000 && rising) || (p>=P1274_FAST && mc>=70000))cap=1261000;
    else if(mc>=70000 || (mc>=65000 && rising) || (p>=P1274_FAST && mc>=67000))cap=1469000;
    return !old?cap:!cap?old:old<cap?old:cap;
}
static inline uint32_t s9_p12741_little_cap(int32_t mc,int32_t big,bool rising,uint32_t old) {
    if(!s9_p1274_temp_valid(mc) || !s9_p1274_temp_valid(big))
        return old && old<1053000u?old:1053000u;
    uint32_t cap=0;
    if(mc>=70000 || big>=76000)cap=1053000u;
    else if(mc>=67000 || (big>=68000 && rising))cap=1248000u;
    return !old?cap:!cap?old:old<cap?old:cap;
}
/* A slope alone is evidence, not a reason to consume the only full
 * per-thread owner ring. Moderate events have their own bounded samples. */
static inline unsigned s9_p12741_severity(unsigned bits,int32_t mc,unsigned sum) {
    if((bits&8u) || (s9_p1274_temp_valid(mc)&&mc>=76000))return 3;
    if((bits&2u) || ((bits&1u) && sum>=200 && mc>=67000))return 2;
    return bits?1u:0u;
}
/* Mode mask: early nil=1, workload cap=2, bounded coordination=4, lazy ICD=8.
 * User-visible mode is 1+mask; zero is old lab/off. */
static inline unsigned s9_p1275_load_level(unsigned sum,bool rising,int32_t big,int32_t lit){
    if(sum>=400 || (sum>=200 && (rising || big>=67000 || lit>=65000)))return 2;
    return sum>=200?1:0;
}
static inline unsigned s9_p1274_trigger(bool eligible,bool sustained,int32_t mc,bool rising) {
    if(!eligible)return 0;
    return (sustained?1u:0u)|((s9_p1274_temp_valid(mc)&&mc>=70000)?2u:0u)|(rising?4u:0u);
}
#endif
