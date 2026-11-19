/* SPDX-License-Identifier: MIT
 * S9 G1: cost of EAGL slot calls, CPU wall-clock microseconds only.
 * No changed GL state/draw calls and no GPU fence or readback.
 */
#ifndef S9_GL_COST_H
#define S9_GL_COST_H
#include <stdint.h>
#include <limits.h>
#include <string.h>
#define S9_GL_COST_SLOTS 0x440u
enum s9_gl_cost_group {
    S9_GL_DRAW=0, S9_GL_TEXTURE=1, S9_GL_UNIFORM=2,
    S9_GL_SHADER=3, S9_GL_STATE=4, S9_GL_VERTEX=5,
    S9_GL_TARGET=6, S9_GL_OTHER=7, S9_GL_GROUPS=8
};
struct s9_gl_cost {
    uint64_t us[S9_GL_COST_SLOTS];
    uint32_t count[S9_GL_COST_SLOTS];
};
static inline unsigned s9_gl_cost_group_of(unsigned offset) {
    /* The entry offsets are from iOS 16.5 eagl_slots.txt, NOT GLES enums. */
    switch(offset) {
      case 0x0210: case 0x0220: case 0x0cb0: case 0x17d8: case 0x17e0:
          return S9_GL_DRAW;
      case 0x0030: case 0x01b8: case 0x01c8: case 0x01e0:
      case 0x0318: case 0x0970: case 0x0978: case 0x0980: case 0x0988:
      case 0x0990: case 0x09a0: case 0x0ab8: case 0x0db8:
      case 0x0dc0: case 0x0dc8: case 0x1550: case 0x1920:
          return S9_GL_TEXTURE;
      case 0x1298: case 0x12a0: case 0x12a8: case 0x12b0: case 0x12b8:
      case 0x12c0: case 0x12c8: case 0x1280: case 0x1390:
      case 0x13b8: case 0x13c8: case 0x19a0: case 0x19b0: case 0x19b8:
          return S9_GL_SHADER;
      case 0x0200: case 0x0248: case 0x0040: case 0x0068: case 0x0080:
      case 0x0190: case 0x07e0: case 0x0a80: case 0x0a88:
      case 0x0a90: case 0x0a98: case 0x0e58:
          return S9_GL_STATE;
      case 0x0a78: case 0x1000: case 0x1008: case 0x1010:
      case 0x1418: case 0x1428: case 0x1438: case 0x1440:
      case 0x16e0: case 0x17d0: case 0x1808: case 0x1818:
          return S9_GL_VERTEX;
      case 0x0058: case 0x02d0: case 0x02d8: case 0x0eb8:
      case 0x1478: case 0x14d8: case 0x14e8: case 0x1508:
      case 0x1510: case 0x1518: case 0x1530: case 0x1540:
      case 0x15a0: case 0x1838: case 0x1840: case 0x1848:
      case 0x1850: case 0x18c0: case 0x18e8:
          return S9_GL_TARGET;
      default:break;
    }
    if ((offset>=0x12d8 && offset<=0x1368) ||
        (offset>=0x16f8 && offset<=0x1738) ||
        (offset>=0x1770 && offset<=0x1798))
        return S9_GL_UNIFORM;
    return S9_GL_OTHER;
}
static inline int s9_gl_cost_add(struct s9_gl_cost *s, unsigned offset, uint64_t duration) {
    unsigned slot=offset/8;
    if (!s || offset%8 || slot>=S9_GL_COST_SLOTS) return 0;
    ++s->count[slot];
    s->us[slot]+=duration;
    return 1;
}
static inline void s9_gl_cost_reset(struct s9_gl_cost *s) {
    memset(s,0,sizeof(*s));
}
static inline void s9_gl_cost_summary(
    const struct s9_gl_cost *s,uint64_t sums[S9_GL_GROUPS],
    uint32_t counts[S9_GL_GROUPS],unsigned top[3]) {
    for(unsigned g=0;g<S9_GL_GROUPS;++g){sums[g]=0;counts[g]=0;}
    for(unsigned i=0;i<3;++i)top[i]=UINT_MAX;
    if(!s)return;
    for(unsigned slot=0;slot<S9_GL_COST_SLOTS;++slot) {
        if(!s->count[slot])continue;
        const unsigned group=s9_gl_cost_group_of(slot*8);
        sums[group]+=s->us[slot];
        counts[group]+=s->count[slot];
        for(unsigned j=0;j<3;++j) {
            if(top[j]==UINT_MAX || s->us[slot] > s->us[top[j]] ||
               (s->us[slot]==s->us[top[j]] && s->count[slot]>s->count[top[j]])) {
                for(unsigned k=2;k>j;--k)top[k]=top[k-1];
                top[j]=slot;
                break;
            }
        }
    }
}
#endif
