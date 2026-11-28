/* Rev9 bounded, context-local accumulators. No policy decisions. */
#ifndef S9_P1279_GL_H
#define S9_P1279_GL_H
#include <stdint.h>
#include <string.h>
enum {P9_SHARE_OK,P9_SHARE_DISABLED,P9_INVERT,P9_ALPHA,P9_FORMAT,P9_PLANE,P9_DIMENSIONS,P9_SHARE_BACKEND,
      P9_MIRROR_DISABLED,P9_MIRROR_NOT_TARGET,P9_MIRROR_BACKEND,P9_MIRROR_OK,P9_SHARE_REASONS};
struct s9_p1279_gl {
    uint64_t first,last,wall,max_wall,attach,zero_wall,zero_hash_bytes,zero_hash_us;
    uint64_t hash_bytes,hash_us,frame_hash_bytes,frame_hash_us,cpu,paired_wall;
    uint64_t share_bytes[P9_SHARE_REASONS],share_us[P9_SHARE_REASONS];
    uint64_t upload_bytes,lock_us,convert_us,teximage_us;
    uint32_t frames,zero_frames,hash_same,hash_dirty,cpu_n,share_n[P9_SHARE_REASONS];
    uint32_t upload_n,flush_n,finish_n,fence_n,blit_n;
    uint32_t backend_n[9],backend_last_error;
    uint64_t backend_bytes[9];
};
static inline void s9_p1279_begin(struct s9_p1279_gl *p,uint64_t now){if(!p->first)p->first=now;}
static inline void s9_p1279_hash(struct s9_p1279_gl *p,uint64_t bytes,uint64_t us,int same){
    p->hash_bytes+=bytes;p->hash_us+=us;p->frame_hash_bytes+=bytes;p->frame_hash_us+=us;
    if(same)p->hash_same++;else p->hash_dirty++;
}
static inline void s9_p1279_share(struct s9_p1279_gl *p,unsigned reason,uint64_t bytes,uint64_t us){
    if(reason>=P9_SHARE_REASONS)return;
    p->share_n[reason]++;p->share_bytes[reason]+=bytes;p->share_us[reason]+=us;
}
static inline void s9_p1279_backend(struct s9_p1279_gl *p,unsigned status,uint64_t bytes){
    unsigned bucket=status<8?status:8;p->backend_n[bucket]++;p->backend_bytes[bucket]+=bytes;p->backend_last_error=status;
}
static inline void s9_p1279_frame(struct s9_p1279_gl *p,uint64_t now,uint64_t wall,uint64_t attach,unsigned uploads,int valid,uint64_t cpu,uint64_t paired){
    if(!p->first)p->first=now;
    p->last=now;p->frames++;p->wall+=wall;p->attach+=attach;if(wall>p->max_wall)p->max_wall=wall;
    if(!uploads){p->zero_frames++;p->zero_wall+=wall;p->zero_hash_bytes+=p->frame_hash_bytes;p->zero_hash_us+=p->frame_hash_us;}
    p->frame_hash_bytes=p->frame_hash_us=0;
    if(valid && cpu<=paired){p->cpu_n++;p->cpu+=cpu;p->paired_wall+=paired;}
}
static inline int s9_p1279_report_due(const struct s9_p1279_gl *p,uint64_t now){return p->frames && now>=p->first && now-p->first>=5000000u;}
static inline void s9_p1279_reset(struct s9_p1279_gl *p,uint64_t now){memset(p,0,sizeof(*p));p->first=now;}
#endif
