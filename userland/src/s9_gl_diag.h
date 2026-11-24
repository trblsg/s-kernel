/* SPDX-License-Identifier: MIT
 * S9 diagnosis only: bounded, sampled CPU wall-time probes for the
 * GLES-to-IOSurface bridge. No GL command, fence, or renderer changes.
 * Samples are 1 in 16 calls; serials are never reset across 60-frame bins.
 */
#ifndef S9_GL_DIAG_H
#define S9_GL_DIAG_H
#include <stdint.h>
#include <string.h>
struct s9_gl_diag {
    uint64_t bind_serial, hash_serial, draw_serial;
    uint64_t bind_native_us, bind_lookup_us, bind_check_us, bind_upload_us;
    uint64_t hash_us, hash_bytes, draw_native_us, draw_other_us, flush_raw_us;
    uint32_t bind_samples, bind_with_image, bind_checks, bind_dirty;
    uint32_t hash_samples, hash_same, hash_changed, hash_not_checked;
    uint32_t draw_samples, draw_native_calls;
};
static inline int s9_gl_diag_sample16(uint64_t *serial) {
    if(!serial) return 0;
    return (++*serial & 15u)==0;
}
/* One sample per block of 16, rotating residue over 256 calls. */
static inline int s9_gl_diag_sample16_rotating(uint64_t *serial) {
    if(!serial)return 0;
    uint64_t n=(*serial)++;
    return (n&15u)==((n>>4)&15u);
}
static inline void s9_gl_diag_reset_window(struct s9_gl_diag *s) {
    if(!s)return;
    const uint64_t b=s->bind_serial,h=s->hash_serial,d=s->draw_serial;
    memset(s,0,sizeof(*s));
    s->bind_serial=b;s->hash_serial=h;s->draw_serial=d;
}
#endif
