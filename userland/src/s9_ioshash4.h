/* SPDX-License-Identifier: MIT
 * S9 P0: four independent 64-bit streams to checksum ALL IOSurface bytes.
 *
 * The old single-dependency hash reads an 8-byte word, multiplies,
 * shifts and XORs before it can begin the next word. The new fast path
 * advances FOUR independent accumulators per 32-byte block, allowing
 * the arm64 backend to overlap the multiplies and memory operations.
 * A straightforward scalar reference computes the EXACT SAME digest.
 *
 * This is a 64-bit NON-CRYPTOGRAPHIC checksum, not a promise of
 * collision-free change detection. The old hash was probabilistic too
 * and skipped a trailing 1..7 bytes; P0 covers those trailing bytes.
 *
 * The hash selection must be fixed at process startup: both the
 * stored per-surface sum and subsequent reads use the same algorithm.
 */
#ifndef S9_IOSHASH4_H
#define S9_IOSHASH4_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S9_IOSHASH4_MULT UINT64_C(0x9e3779b97f4a7c15)
static inline uint64_t s9_ioshash4_step(uint64_t h, uint64_t word, unsigned bytes) {
    h = (h ^ word ^ ((uint64_t)bytes << 56)) * S9_IOSHASH4_MULT;
    return h ^ (h >> 29);
}
static inline void s9_ioshash4_init(uint64_t lane[4], size_t size) {
    lane[0] = UINT64_C(0x243f6a8885a308d3) ^ (uint64_t)size;
    lane[1] = UINT64_C(0x13198a2e03707344) ^ ((uint64_t)size << 1);
    lane[2] = UINT64_C(0xa4093822299f31d0) ^ ((uint64_t)size << 2);
    lane[3] = UINT64_C(0x082efa98ec4e6c89) ^ ((uint64_t)size << 3);
}
static inline uint64_t s9_ioshash4_finish(const uint64_t lane[4], size_t size) {
    uint64_t out = ((uint64_t)size ^ UINT64_C(0x452821e638d01377));
    for (unsigned i = 0; i < 4; ++i) {
        out = (out ^ lane[i]) * UINT64_C(0xbf58476d1ce4e5b9);
        out ^= out >> 31;
    }
    return out;
}
/* Portable C11 reference used by tests and on-device sampled verification. */
static inline uint64_t s9_ioshash4_reference(const void *data, size_t size) {
    const uint8_t *base = (const uint8_t *)data;
    uint64_t lane[4];
    s9_ioshash4_init(lane, size);
    for (size_t at = 0, index = 0; at < size; ++index) {
        size_t take = size - at < 8 ? size - at : 8;
        uint64_t word = 0;
        memcpy(&word, base + at, take);
        lane[index & 3] = s9_ioshash4_step(lane[index & 3], word, (unsigned)take);
        at += take;
    }
    return s9_ioshash4_finish(lane, size);
}
/* Fast implementation: four independent loads and hash lanes per chunk. */
static inline uint64_t s9_ioshash4_fast(const void *data, size_t size) {
    const uint8_t *base = (const uint8_t *)data;
    uint64_t lane[4];
    size_t at = 0;
    unsigned index = 0;
    s9_ioshash4_init(lane, size);
    for (; size - at >= 32; at += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, base + at, 8);
        memcpy(&w1, base + at + 8, 8);
        memcpy(&w2, base + at + 16, 8);
        memcpy(&w3, base + at + 24, 8);
        lane[0] = s9_ioshash4_step(lane[0], w0, 8);
        lane[1] = s9_ioshash4_step(lane[1], w1, 8);
        lane[2] = s9_ioshash4_step(lane[2], w2, 8);
        lane[3] = s9_ioshash4_step(lane[3], w3, 8);
    }
    for (; at < size; index++) {
        const size_t take = size - at < 8 ? size - at : 8;
        uint64_t word = 0;
        memcpy(&word, base + at, take);
        lane[index & 3] = s9_ioshash4_step(lane[index & 3], word, (unsigned)take);
        at += take;
    }
    return s9_ioshash4_finish(lane, size);
}
#endif
