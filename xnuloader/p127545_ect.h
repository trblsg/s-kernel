/* ECT MINLOCK extraction, read-only; format facts from Apollo 30656faf0758
 * drivers/soc/samsung/ect_parser.c. No clock-setting authority.
 * The source must be an allocated, readable physical ECT region, not a
 * guessed pointer. Export only compact bounded tables to /chosen.
 */
#ifndef P127545_ECT_H
#define P127545_ECT_H
#include <stdint.h>
#include <stddef.h>
#include "../drivers/soc/exynos/hakios.p127545_ect_abi.h"
#define P545_ECT_MAX_DIR 256u
static uint32_t p545_ect_u32(const volatile uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static int p545_ect_eq(const volatile uint8_t *p,uint32_t n,const char *s){
    for(uint32_t i=0;i<n;i++){if(!s[i] || p[i]!=(uint8_t)s[i])return 0;}
    return s[n]==0;
}
/* Directory offsets are relative to the block, names length excludes NUL
 * and occupies a four-byte-aligned field. Bound selected block by the next
 * offset, regardless of directory ordering. */
struct p545_ect_entry {uint32_t name,len,off;};
static int p545_ect_entry(const volatile uint8_t *d,uint32_t size,uint32_t *pos,
                          struct p545_ect_entry *e){
    uint32_t at=*pos;
    if(at>size || size-at<4)return 0;
    uint32_t n=p545_ect_u32(d+at);at+=4;
    if(!n || n>128)return 0;
    uint32_t padded=(n+4u)&~3u;
    if(size-at<padded+4u || d[at+n])return 0;
    for(uint32_t j=0;j<n;j++)if(!d[at+j])return 0;
    e->name=at;e->len=n;e->off=p545_ect_u32(d+at+padded);
    if((e->off&3u)||e->off>=size)return 0;
    *pos=at+padded+4;
    return 1;
}
static int p545_ect_find(const volatile uint8_t *d,uint32_t size,uint32_t pos,uint32_t count,
                        const char *name,uint32_t *start,uint32_t *end){
    /* Re-scan the bounded directory instead of allocating 3 KiB of
     * scratch on the UEFI stack. No large stack frame or dynamic memory. */
    if(!count || count>P545_ECT_MAX_DIR)return 0;
    uint32_t first=pos,match=0;
    for(uint32_t i=0;i<count;i++){
        struct p545_ect_entry e;
        if(!p545_ect_entry(d,size,&pos,&e))return 0;
        if(p545_ect_eq(d+e.name,e.len,name)){match++;*start=e.off;}
        uint32_t prior=first;
        for(uint32_t j=0;j<i;j++){
            struct p545_ect_entry a;
            if(!p545_ect_entry(d,size,&prior,&a) || a.off==e.off)return 0;
            if(a.len==e.len){
                uint32_t k=0;while(k<e.len && d[a.name+k]==d[e.name+k])k++;
                if(k==e.len)return 0;
            }
        }
    }
    if(match!=1)return 0;
    *end=size;
    uint32_t limit=pos;pos=first;
    for(uint32_t i=0;i<count;i++){
        struct p545_ect_entry e;
        if(!p545_ect_entry(d,size,&pos,&e) || e.off<limit)return 0;
        if(e.off>*start && e.off<*end)*end=e.off;
    }
    return 1;
}
static int p545_ect_rows(const volatile uint8_t *d,uint32_t size,struct p545_ect_pair *out,uint32_t *count){
    if(size<4)return 0;
    uint32_t n=p545_ect_u32(d);
    if(!n || n>P545_ECT_MAX_ROWS || n>(size-4)/8)return 0;
    for(uint32_t i=0;i<n;i++){
        uint32_t x=p545_ect_u32(d+4+8*i),y=p545_ect_u32(d+8+8*i);
        if(x<1000 || x>4000000 || y>4000000 || (y && y<1000))return 0;
        for(uint32_t j=0;j<i;j++)if(out[j].master==x)return 0;
        out[i].master=x;out[i].slave=y;
    }
    *count=n;return 1;
}
static int p545_ect_extract(const volatile uint8_t *d,uint32_t cap,struct p545_ect_snapshot *out){
    out->abi=0;
    if(cap<16 || !p545_ect_eq(d,4,"PARA"))return 0;
    uint32_t size=p545_ect_u32(d+8),start=0,end=0;
    if(size<16 || size>cap || !p545_ect_find(d,size,16,p545_ect_u32(d+12),"MINLOCK",&start,&end) || end-start<12)return 0;
    const volatile uint8_t *m=d+start;uint32_t len=end-start;
    if(p545_ect_u32(m)>16)return 0;
    uint32_t n=p545_ect_u32(m+8),a=0,b=0;
    if(!p545_ect_find(m,len,12,n,"dvfs_mif",&a,&b) || !p545_ect_rows(m+a,b-a,out->mif,&out->mif_n))return 0;
    if(!p545_ect_find(m,len,12,n,"dvfs_int",&a,&b) || !p545_ect_rows(m+a,b-a,out->intr,&out->int_n))return 0;
    /* FNV-1a is a corruption fingerprint, not firmware authentication. */
    uint32_t hash=2166136261u;
    for(uint32_t i=0;i<size;i++)hash=(hash^d[i])*16777619u;
    out->size=size;out->version=p545_ect_u32(d+4);out->fingerprint=hash;out->abi=1;
    return 1;
}
#endif
