#ifndef HAKIOS_P1278_PANEL_H
#define HAKIOS_P1278_PANEL_H
#include <stdint.h>
#include <stdbool.h>
/* Hardware constants transcribed from Samsung's S0/S1 WQHD tables,
 * pinned e5906667068efe49ad6a5494296f77965f2e5c5e. See docs/s9-p1278.md.
 * Both variants' 128-byte DSC decoder PPS are identical. No vendor power,
 * oscillator, gamma calibration, reset or supply writes here. */
static const uint8_t p1278_wqhd_pps[128]={
 0x11,0x00,0x00,0x89,0x30,0x80,0x0B,0x90,
 0x05,0xA0,0x00,0x28,0x02,0xD0,0x02,0xD0,
 0x02,0x00,0x02,0x68,0x00,0x20,0x04,0x6C,
 0x00,0x0A,0x00,0x0C,0x02,0x77,0x01,0xE9,
 0x18,0x00,0x10,0xF0,0x03,0x0C,0x20,0x00,
 0x06,0x0B,0x0B,0x33,0x0E,0x1C,0x2A,0x38,
 0x46,0x54,0x62,0x69,0x70,0x77,0x79,0x7B,
 0x7D,0x7E,0x01,0x02,0x01,0x00,0x09,0x40,
 0x09,0xBE,0x19,0xFC,0x19,0xFA,0x19,0xF8,
 0x1A,0x38,0x1A,0x78,0x1A,0xB6,0x2A,0xF6,
 0x2B,0x34,0x2B,0x74,0x3B,0x74,0x6B,0xF4
};
struct p1278_dpu_state {
    uint32_t path,connection,start,end,src_size,src_offset,img_size,scaled_size,in_con,dpp_in,base,frame,trigger,shadow;
    uint32_t path0,path1,bg0,bg1,err_i,err_d;
    uint32_t enc[2][17]; /* ctrl0, ctrl3, PPS04..59 (14 words), PPS00 */
};
struct p1278_link_state {uint32_t config,resol,cprs,slice01,slice23,mask,irq,fifo,phy;};
static inline bool p1278_panel_id_ok(uint32_t id){return id==0x910443 || id==0x910453 || id==0x910444;}
static inline bool p1278_link_wqhd(const struct p1278_link_state *s){
    return (s->config & ((1u<<19)|(1u<<18)))==(1u<<19) &&
        (s->config&0x7fu)==0x7fu &&
        (s->resol&0x1fff1fffu)==((2960u<<16)|480u) &&
        (s->cprs&7u)==2 && (s->slice01&0x1fff1fffu)==((720u<<16)|720u);
}
static inline bool p1278_dpu_wqhd(const struct p1278_dpu_state *s){
    if(s->start!=0 || (s->end&0x3fff3fffu)!=((2959u<<16)|1439u) ||
       s->src_offset!=0 || (s->scaled_size&0x3fff3fffu)!=((2960u<<16)|1440u) ||
       (s->path&0x0bu)!=1 || (s->connection&7u)!=0 || s->err_i || s->err_d)return false;
    unsigned n=(s->path>>4)&3u;
    if(n!=1 && n!=3)return false;
    if((n==3)!=((s->path&(1u<<7))!=0))return false;
    for(unsigned i=0;i<2;i++)if(n&(1u<<i)){
        const uint32_t *e=s->enc[i];
        /* Encoder width can be half the decoder picture when DSCC is used.
         * Validate real geometry, 8bpp, two slices and exact chunk width. */
        if((e[0]&(1u<<3)) || (e[2]&0x00ffffffu)!=((128u<<16)|2960u) ||
           e[3]!=(((n==3?720u:1440u)<<16)|40u) ||
           e[4]!=((720u<<16)|720u) || ((e[0]&1u)!= (n==1)))return false;
    }
    return true;
}
static inline bool p1278_link_same(const struct p1278_link_state *a,const struct p1278_link_state *b){
    return a->config==b->config && a->resol==b->resol && a->cprs==b->cprs &&
           a->slice01==b->slice01 && a->slice23==b->slice23;
}
static inline bool p1278_dpu_same(const struct p1278_dpu_state *a,const struct p1278_dpu_state *b){
    if(a->path!=b->path || a->connection!=b->connection || a->start!=b->start || a->end!=b->end ||
       a->src_offset!=b->src_offset || a->img_size!=b->img_size || a->scaled_size!=b->scaled_size ||
       a->in_con!=b->in_con || a->dpp_in!=b->dpp_in)return false;
    for(unsigned i=0;i<2;i++)for(unsigned j=0;j<17;j++)if(a->enc[i][j]!=b->enc[i][j])return false;
    return true;
}
bool hakios_p1278_dpu_capture(struct p1278_dpu_state *s);
bool hakios_p1278_dpu_hold_begin(void);
bool hakios_p1278_dpu_hold_end(void);
bool hakios_p1278_dpu_full_latched(uint32_t gen,uint32_t *frame);
#endif
