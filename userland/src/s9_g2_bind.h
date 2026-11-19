/* SPDX-License-Identifier: MIT
 * G2: skip a redundant GL_TEXTURE_2D driver bind only when the last
 * observed native binding on that context/unit is the same name.
 * All intercepted calls STILL perform IOSurface freshness/seed checks.
 * Never assume a binding remains valid after an internal raw GL bind.
 */
#ifndef S9_G2_BIND_H
#define S9_G2_BIND_H
#include <stdint.h>
#include <string.h>
#define S9_G2_UNITS 8u
struct s9_g2_state {
    uint32_t name[S9_G2_UNITS];
    uint8_t valid[S9_G2_UNITS];
    uint32_t attempts, skipped, executed, invalidations;
    uint64_t verify_serial; /* persists across 60-frame counter resets */
};
static inline void s9_g2_invalidate(struct s9_g2_state *s) {
    if (!s) return;
    memset(s->valid, 0, sizeof(s->valid));
    ++s->invalidations;
}
static inline int s9_g2_redundant(const struct s9_g2_state *s,
                                  unsigned unit, unsigned target,
                                  unsigned name, unsigned tex2d) {
    return s && unit < S9_G2_UNITS && target == tex2d &&
           s->valid[unit] && s->name[unit] == name;
}
static inline void s9_g2_record(struct s9_g2_state *s,
                                unsigned unit, unsigned target,
                                unsigned name, unsigned tex2d) {
    if (!s) return;
    if (unit < S9_G2_UNITS && target == tex2d) {
        s->valid[unit] = 1;
        s->name[unit] = name;
    } else {
        s9_g2_invalidate(s);
    }
}
static inline void s9_g2_reset_counters(struct s9_g2_state *s) {
    if (!s) return;
    s->attempts = s->skipped = s->executed = s->invalidations = 0;
}
#endif
