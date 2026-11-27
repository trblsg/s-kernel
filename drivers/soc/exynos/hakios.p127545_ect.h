/* Read only the bounded loader snapshot, never map the physical ECT
 * address in XNU. The loader may have refused capture. No SET authority. */
#include <pexpert/device_tree.h>
#include "hakios.p127545_ect_abi.h"
#include "hakios.p127545_int_bounds.h"
static bool p545_ect_valid_rows(const struct p545_ect_pair *p,uint32_t n){
    if(!n || n>P545_ECT_MAX_ROWS)return false;
    for(uint32_t i=0;i<n;i++){
        if(p[i].master<1000 || p[i].master>4000000 ||
           p[i].slave>4000000 || (p[i].slave && p[i].slave<1000))return false;
        for(uint32_t j=0;j<i;j++)if(p[i].master==p[j].master)return false;
    }
    return true;
}
static bool p545_ect_get(struct p545_ect_snapshot *out){
    DTEntry node;const void *prop;unsigned int size;
    if(SecureDTLookupEntry(NULL,"/chosen",&node)!=kSuccess ||
       SecureDTGetProperty(node,"hakios-ect-minlock",&prop,&size)!=kSuccess ||
       size!=sizeof(*out))return false;
    memcpy(out,prop,sizeof(*out));
    return out->abi==1 && out->size>=16 && out->size<=0x32000 &&
        p545_ect_valid_rows(out->mif,out->mif_n) && p545_ect_valid_rows(out->intr,out->int_n);
}
static int p545_ect_sysctl(__unused struct sysctl_oid *o,__unused void *a,int table,struct sysctl_req *req){
    struct p545_ect_snapshot s;
    char out[1024];bool ok=p545_ect_get(&s);
    uint32_t n=ok?(table==0?s.mif_n:s.int_n):0;
    const struct p545_ect_pair *rows=table==0?s.mif:s.intr;
    int used=snprintf(out,sizeof(out),"version=1 valid=%u table=%u size=%u fingerprint=%u count=%u live_verified=0 write_allowed=0 rows=",
        ok,table,ok?s.size:0,ok?s.fingerprint:0,n);
    if(!n){out[used++]='-';out[used]=0;}
    for(uint32_t i=0;i<n;i++){
        if(used<0 || (size_t)used>=sizeof(out))return EOVERFLOW;
        int len=snprintf(out+used,sizeof(out)-(size_t)used,"%s%u:%u",i?",":"",rows[i].master,rows[i].slave);
        if(len<0 || (size_t)len>=sizeof(out)-(size_t)used)return EOVERFLOW;
        used+=len;
    }
    return SYSCTL_OUT(req,out,(size_t)used+1);
}
/* Copy the actual firmware rate/voltage table with bounds, and compare
 * two snapshots under the common ACPM lock. No interpretation of init_lv
 * as live state. Only min/max feasibility across all advertised OPPs. */
static bool p545_int_fv_rates(uint32_t map,uint32_t domain,uint32_t *v,uint32_t *n){
    if(!map || !sram_ok(map,13u*FV_HEADER))return false;
    uint32_t h=map+domain*FV_HEADER,count=srd8(h+FV_NUM_LV);
    uint32_t offset=srd16(h+FV_O_RATEVOLT);
    if(!count || count>P545_ECT_MAX_ROWS || map+offset<map ||
       !sram_ok(map+offset,count*8u))return false;
    for(uint32_t i=0;i<count;i++){
        uint32_t f=srd(map+offset+i*8u),uv=srd(map+offset+i*8u+4u);
        if(uv<300000 || uv>1500000 || f<1000 || f>4000000 || (i && f>=v[i-1]))return false;
        v[i]=f;
    }
    *n=count;return true;
}
struct p545_int_fv {uint32_t nm,ni,nf,mif[32],intr[32],fsys[32];};
static bool p545_int_fv_snapshot(struct p545_int_fv *s){
    uint32_t map=fvmap_at(printf);
    return p545_int_fv_rates(map,0,s->mif,&s->nm) &&
        p545_int_fv_rates(map,1,s->intr,&s->ni) &&
        p545_int_fv_rates(map,6,s->fsys,&s->nf);
}
static int p545_int_bounds_sysctl(__unused struct sysctl_oid *o,__unused void *a,__unused int b,struct sysctl_req *req){
    struct p545_ect_snapshot ect;
    struct p545_int_fv s={0},again={0};
    struct p545_int_bounds result={0};
    bool valid=p545_ect_get(&ect),stable=false;
    lck_mtx_lock(&acpm_lock);
    if(acpm_ready && p545_int_fv_snapshot(&s)){
        __builtin_arm_dsb(15);
        stable=p545_int_fv_snapshot(&again) && !memcmp(&s,&again,sizeof(s));
    }
    lck_mtx_unlock(&acpm_lock);
    if(valid && stable)result=p545_int_bound(&ect,s.mif,s.nm,s.intr,s.ni,s.fsys,s.nf);
    char out[256];
    snprintf(out,sizeof(out),"version=1 ect=%u fv_stable=%u complete=%u mif_floor=%u fsys0_floor=%u stage_mask=%u live_verified=0 write_allowed=0",
        valid,stable,result.complete,result.mif_floor,result.fsys0_floor,result.stage_mask);
    return SYSCTL_OUT(req,out,strlen(out)+1);
}
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127545_int_bounds,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p545_int_bounds_sysctl,"A","Worst-case ECT/FVMAP constraint feasibility; never write authority");
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127545_ect_mif,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,0,p545_ect_sysctl,"A","ECT MIF to INT minlock; read-only snapshot");
SYSCTL_PROC(_kern_hakios,OID_AUTO,p127545_ect_int,CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_LOCKED|CTLFLAG_KERN,NULL,1,p545_ect_sysctl,"A","ECT INT to FSYS0 minlock; read-only snapshot");
