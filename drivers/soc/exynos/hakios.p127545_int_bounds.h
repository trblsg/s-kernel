/* Pure conservative MINLOCK check. Every firmware OPP must have an
 * exact dependency pair; no interpolation or assuming the boot OPP is live.
 * The result is only table feasibility, never permission to SET. */
#ifndef HAKIOS_P545_INT_BOUNDS_H
#define HAKIOS_P545_INT_BOUNDS_H
#include "hakios.p127545_ect_abi.h"
#include <stdbool.h>
struct p545_int_bounds {
    uint32_t mif_floor,fsys0_floor,stage_mask;
    bool complete;
};
static inline bool p545_int_lookup(const struct p545_ect_pair *rows,uint32_t n,
                                    uint32_t master,uint32_t *slave){
    uint32_t hits=0,value=0;
    if(!n || n>P545_ECT_MAX_ROWS)return false;
    for(uint32_t i=0;i<n;i++)if(rows[i].master==master){hits++;value=rows[i].slave;}
    if(hits!=1 || !value)return false;
    *slave=value;return true;
}
static inline bool p545_int_has(const uint32_t *list,uint32_t n,uint32_t value){
    for(uint32_t i=0;i<n;i++)if(list[i]==value)return true;
    return false;
}
static inline bool p545_int_opp_list(const uint32_t *v,uint32_t n){
    if(!n || n>P545_ECT_MAX_ROWS)return false;
    for(uint32_t i=0;i<n;i++)if(v[i]<1000 || v[i]>4000000 || (i && v[i]>=v[i-1]))return false;
    return true;
}
static inline struct p545_int_bounds p545_int_bound(
    const struct p545_ect_snapshot *ect,const uint32_t *mif,uint32_t nm,
    const uint32_t *intr,uint32_t ni,const uint32_t *fsys,uint32_t nf){
    struct p545_int_bounds r={0};
    if(!p545_int_opp_list(mif,nm) || !p545_int_opp_list(intr,ni) ||
       !p545_int_opp_list(fsys,nf) || ni!=6)return r;
    const uint32_t stage[]={534000,400000,336000,267000,178000,107000};
    for(uint32_t i=0;i<ni;i++)if(intr[i]!=stage[i])return r;
    uint32_t mif_floor=0,need[6]={0};
    for(uint32_t i=0;i<nm;i++){
        uint32_t v=0;
        if(!p545_int_lookup(ect->mif,ect->mif_n,mif[i],&v) || !p545_int_has(intr,ni,v))return r;
        if(v>mif_floor)mif_floor=v;
    }
    for(uint32_t i=0;i<ni;i++)
        if(!p545_int_lookup(ect->intr,ect->int_n,intr[i],&need[i]) || !p545_int_has(fsys,nf,need[i]))return r;
    r.complete=true;r.mif_floor=mif_floor;r.fsys0_floor=fsys[nf-1];
    for(uint32_t i=0;i<ni;i++)
        if(intr[i]>=mif_floor && need[i]<=r.fsys0_floor)r.stage_mask|=1u<<i;
    return r;
}
#endif
