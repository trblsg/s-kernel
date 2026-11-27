/* Exynos9810 CMU_TOP clock topology, facts from Apollo 30656faf0758
 * cal-if/exynos9810/cmucal-{node,sfr}.c. Calculated selected clock rates,
 * NOT ACPM domain readback, actual clock residency or MIF min-lock proof.
 * All registers are read twice by caller; reject changing or busy path.
 */
#ifndef HAKIOS_P127545_CMU_H
#define HAKIOS_P127545_CMU_H
#include <stdint.h>
#define P545_BUSY (1u << 16)
#define P545_PLL_OK ((1u << 31) | (1u << 29))
/* Shared0..4, div0/2, div0/4, div1/2, div1/4, div1/3, div2/2, div4/2,
 * BUSC mux/div, BUS1 mux/div, DPU final mux, primary div, alternate
 * mux/div, MIF PLL, DDRPHY2X mux. TOP and MIF are always accessible
 * on the supported Exynos9810 platform; no PD-local MMIO here. */
enum p545_reg {
 P545_PLL0,P545_PLL1,P545_PLL2,P545_PLL3,P545_PLL4,
 P545_D02,P545_D04,P545_D12,P545_D14,P545_D13,P545_D22,P545_D42,
 P545_BUSC_M,P545_BUSC_D,P545_BUS1_M,P545_BUS1_D,
 P545_DPU_M,P545_DPU_D,P545_DPU_ALT_M,P545_DPU_ALT_D,
 P545_FSYS0_M,P545_FSYS0_D,
 P545_MIF_PLL,P545_MIF_M,P545_NREG
};
static const uint16_t p545_offsets[P545_NREG]={
 0x0120,0x01a0,0x0220,0x02a0,0x0320,
 0x18d4,0x18dc,0x18e0,0x18e8,0x18e4,0x18ec,0x18f4,
 0x1010,0x180c,0x100c,0x1808,
 0x1000,0x18c8,0x1048,0x18cc,
 0x1054,0x184c,
 0x0120,0x1000
};
struct p545_cmu_rates {uint32_t busc,bus1,disp,mif_pll,ddrphy2x,fsys0;};
static inline uint32_t p545_pll(uint32_t c){
 uint32_t m=(c>>16)&0x3ff,p=(c>>8)&0x3f,s=c&7;
 if((c&P545_PLL_OK)!=P545_PLL_OK || !p || !m)return 0;
 return (uint32_t)(26000ull*m/((uint64_t)p<<s));
}
static inline uint32_t p545_div(uint32_t rate,uint32_t c,uint32_t mask){
 return c&P545_BUSY?0:rate/((c&mask)+1);
}
static inline uint32_t p545_mux(const uint32_t *parents,uint32_t c,uint32_t mask){
 return c&P545_BUSY?0:parents[c&mask];
}
static inline struct p545_cmu_rates p545_decode(const uint32_t r[P545_NREG]){
 uint32_t p0=p545_pll(r[P545_PLL0]),p1=p545_pll(r[P545_PLL1]);
 uint32_t p2=p545_pll(r[P545_PLL2]),p3=p545_pll(r[P545_PLL3]),p4=p545_pll(r[P545_PLL4]);
 uint32_t d02=p545_div(p0,r[P545_D02],1),d12=p545_div(p1,r[P545_D12],1);
 uint32_t d22=p545_div(p2,r[P545_D22],1);
 uint32_t bus[]={p545_div(d02,r[P545_D04],1),p545_div(d12,r[P545_D14],1),d22,p545_div(p4,r[P545_D42],1)};
 uint32_t alt[]={p3,p4,p545_div(p1,r[P545_D13],3),d22};
 uint32_t fsys[]={d12,p2,p4,bus[0]};
 uint32_t disp[]={p545_div(d02,r[P545_DPU_D],7),p545_div(p545_mux(alt,r[P545_DPU_ALT_M],3),r[P545_DPU_ALT_D],15)};
 struct p545_cmu_rates out={
  p545_div(p545_mux(bus,r[P545_BUSC_M],3),r[P545_BUSC_D],15),
  p545_div(p545_mux(bus,r[P545_BUS1_M],3),r[P545_BUS1_D],15),
  p545_mux(disp,r[P545_DPU_M],1),p545_pll(r[P545_MIF_PLL]),0,
  p545_div(p545_mux(fsys,r[P545_FSYS0_M],3),r[P545_FSYS0_D],15)
 };
 /* PLL-selected DDRPHY2X only. The switch path is intentionally unknown.
  * DDRPHY2X is not the effective DDR/DRAM operating rate. */
 if(!(r[P545_MIF_M]&(P545_BUSY|1u)))out.ddrphy2x=out.mif_pll;
 return out;
}
#endif
