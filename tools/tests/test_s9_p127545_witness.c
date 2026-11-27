/* Portable decoder tests, independent of XNU and real hardware.
 * cc -std=c11 -Wall -Wextra -Werror tools/tests/test_s9_p127545_witness.c -o /tmp/test-p127545 && /tmp/test-p127545
 */
#include <assert.h>
#include <string.h>
#include "../../drivers/soc/exynos/hakios.p127545_cmu.h"
#include "../../drivers/bluetooth/bcm4361/hakios.p127545_hci.h"
#include "../../xnuloader/p127545_ect.h"
#include "../../drivers/soc/exynos/hakios.p127545_int_bounds.h"

static uint32_t pll(unsigned m,unsigned p,unsigned s){return P545_PLL_OK|(m<<16)|(p<<8)|s;}
static void clock_test(void){
    uint32_t r[P545_NREG]={0};
    r[P545_PLL0]=pll(328,4,0);
    r[P545_PLL2]=pll(400,13,0);
    r[P545_D02]=1;r[P545_D04]=1;r[P545_D22]=1;
    r[P545_BUSC_M]=0;r[P545_BUS1_M]=2;
    r[P545_DPU_M]=1;r[P545_DPU_ALT_M]=3;
    r[P545_MIF_PLL]=pll(552,4,0);
    r[P545_FSYS0_M]=1;r[P545_FSYS0_D]=3;
    assert(p545_offsets[P545_DPU_ALT_D]==0x18cc);
    struct p545_cmu_rates x=p545_decode(r);
    assert(x.busc==533000 && x.bus1==400000 && x.disp==400000);
    assert(x.mif_pll==3588000 && x.ddrphy2x==3588000 && x.fsys0==200000);
    r[P545_FSYS0_D]|=P545_BUSY;assert(p545_decode(r).fsys0==0);
    r[P545_FSYS0_D]=3;
    r[P545_DPU_ALT_D]=1;assert(p545_decode(r).disp==200000);
    r[P545_DPU_ALT_D]=2;assert(p545_decode(r).disp==133333);
    r[P545_DPU_ALT_D]|=P545_BUSY;assert(p545_decode(r).disp==0);
    r[P545_DPU_M]=0;r[P545_DPU_D]=1;assert(p545_decode(r).disp==533000);
    r[P545_PLL0]&=~(1u<<29);assert(p545_decode(r).busc==0);
    r[P545_MIF_M]=1;assert(p545_decode(r).ddrphy2x==0);
    r[P545_MIF_M]=P545_BUSY;assert(p545_decode(r).ddrphy2x==0);
}
#define MARK(att,seq,gen) ((struct p545_hci_mark){.irq=(seq),.generation=(gen),.attached=(att),.irq_pending=(att),.known=true})
static void hci_test(void){
    struct p545_hci s={0};
    const uint8_t version[]={1,1,0x10,0},features[]={1,3,0x10,0};
    const uint8_t complete[]={4,0x0e,12,1,1,0x10,0,6,1,0,6,1,0,2,0};
    const uint8_t fcomplete[]={4,0x0e,12,1,3,0x10,0,1,2,3,4,5,6,7,8};
    const uint8_t acl[]={2,1,0,3,0,0x04,0x0e,0x01};
    p545_hci_feed(&s,false,version,1,MARK(true,10,2));
    p545_hci_feed(&s,false,version+1,3,MARK(true,10,2));
    p545_hci_feed(&s,true,acl,sizeof(acl),MARK(true,11,2));
    p545_hci_feed(&s,true,complete,6,MARK(true,11,2));
    p545_hci_feed(&s,true,complete+6,sizeof(complete)-6,MARK(true,11,2));
    assert(!s.invalid && s.op[0].tx==1 && s.op[0].complete==1 && s.op[0].irq==1);
    p545_hci_feed(&s,false,features,sizeof(features),MARK(false,11,4));
    p545_hci_feed(&s,true,fcomplete,sizeof(fcomplete),MARK(false,11,4));
    assert(!s.invalid && s.op[1].complete==1 && s.op[1].poll==1);
    p545_hci_feed(&s,true,fcomplete,sizeof(fcomplete),MARK(false,11,4));
    assert(s.invalid); /* unsolicited completion never qualifies */
    memset(&s,0,sizeof(s));
    p545_hci_feed(&s,false,features,sizeof(features),MARK(true,12,6));
    p545_hci_feed(&s,false,features,sizeof(features),MARK(true,12,6));
    assert(s.invalid);
    memset(&s,0,sizeof(s));
    p545_hci_feed(&s,false,version,sizeof(version),MARK(true,12,6));
    uint8_t error[sizeof(complete)];memcpy(error,complete,sizeof(error));error[6]=1;
    p545_hci_feed(&s,true,error,sizeof(error),MARK(true,13,6));
    assert(s.op[0].complete==0 && s.op[0].irq==0);
    memset(&s,0,sizeof(s));
    p545_hci_feed(&s,false,version,sizeof(version),MARK(true,12,6));
    p545_hci_feed(&s,true,complete,sizeof(complete),MARK(true,13,10));
    assert(!s.invalid && s.op[0].complete==1 && s.op[0].irq==0 && s.op[0].poll==0);
    memset(&s,0,sizeof(s));
    p545_hci_feed(&s,false,version,sizeof(version),MARK(true,12,6));
    struct p545_hci_mark m=MARK(true,13,6);m.irq_pending=false;
    p545_hci_feed(&s,true,complete,sizeof(complete),m);
    assert(s.op[0].complete==1 && s.op[0].irq==0);
    memset(&s,0,sizeof(s));
    p545_hci_feed(&s,false,version,sizeof(version),MARK(true,12,6));
    p545_hci_feed(&s,true,complete,6,MARK(true,13,6));
    p545_hci_feed(&s,true,complete+6,sizeof(complete)-6,MARK(true,14,8));
    assert(s.op[0].complete==1 && s.op[0].irq==0);
}
static void put32(uint8_t *p,uint32_t v){
    for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));
}
static unsigned entry(uint8_t *p,const char *name,uint32_t off){
    unsigned n=(unsigned)strlen(name),padded=(n+4u)&~3u;
    put32(p,n);memcpy(p+4,name,n);put32(p+4+padded,off);
    return 8+padded;
}
static void ect_test(void){
    uint8_t d[256]={0};
    memcpy(d,"PARA",4);put32(d+8,sizeof(d));put32(d+12,1);
    entry(d+16,"MINLOCK",32);
    uint8_t *m=d+32;put32(m,1);put32(m+8,2);
    unsigned pos=12;pos+=entry(m+pos,"dvfs_mif",56);pos+=entry(m+pos,"dvfs_int",80);
    assert(pos<=56);
    put32(m+56,2);put32(m+60,1794000);put32(m+64,400000);
    put32(m+68,421000);put32(m+72,107000);
    put32(m+80,1);put32(m+84,400000);put32(m+88,200000);
    struct p545_ect_snapshot s={0};
    assert(p545_ect_extract(d,sizeof(d),&s) && s.abi==1 && s.mif_n==2 && s.int_n==1);
    assert(s.mif[0].master==1794000 && s.mif[1].slave==107000 && s.intr[0].slave==200000);
    assert(!p545_ect_extract(d,sizeof(d)-1,&s) && !s.abi);
    put32(m+68,1794000);assert(!p545_ect_extract(d,sizeof(d),&s));
    put32(m+68,421000);
    put32(m+56,33);assert(!p545_ect_extract(d,sizeof(d),&s));
    put32(m+56,2);
    /* Invalid directory offset: points into the directory itself. */
    put32(d+28,16);assert(!p545_ect_extract(d,sizeof(d),&s));
    put32(d+28,32);assert(p545_ect_extract(d,sizeof(d),&s));
    /* Every byte truncation of the stated total size fails. */
    for(unsigned n=0;n<sizeof(d);n++)assert(!p545_ect_extract(d,n,&s));
}
static void int_bounds_test(void){
    const uint32_t mif[]={1794000,421000},intr[]={534000,400000,336000,267000,178000,107000},fsys[]={336000,73000};
    struct p545_ect_snapshot s={0};
    s.mif_n=2;s.int_n=6;
    s.mif[0]=(struct p545_ect_pair){1794000,400000};
    s.mif[1]=(struct p545_ect_pair){421000,107000};
    for(unsigned i=0;i<6;i++)s.intr[i]=(struct p545_ect_pair){intr[i],73000};
    struct p545_int_bounds b=p545_int_bound(&s,mif,2,intr,6,fsys,2);
    assert(b.complete && b.mif_floor==400000 && b.fsys0_floor==73000 && b.stage_mask==3);
    s.intr[1].slave=336000;b=p545_int_bound(&s,mif,2,intr,6,fsys,2);
    assert(b.complete && b.stage_mask==1);
    s.mif_n=1;assert(!p545_int_bound(&s,mif,2,intr,6,fsys,2).complete);
    s.mif_n=2;s.mif[1].slave=0;assert(!p545_int_bound(&s,mif,2,intr,6,fsys,2).complete);
    s.mif[1].slave=267000;s.mif[1].master=1794000;
    assert(!p545_int_bound(&s,mif,2,intr,6,fsys,2).complete);
}
int main(void){clock_test();hci_test();ect_test();int_bounds_test();return 0;}
