/* SPDX-License-Identifier: MIT
 * P4: conservative thread-delta arithmetic for libproc's thread counters.
 * The API's exported time unit can differ by XNU build. Caller logs raw
 * deltas plus both mach-ticks and nanoseconds interpretations; never
 * silently assumes one or promises per-thread wakeup events.
 */
#ifndef S9_P4_THREAD_DELTA_H
#define S9_P4_THREAD_DELTA_H
#include <stdint.h>
static inline uint64_t s9_p4_delta(uint64_t before,uint64_t after) {
    return after>=before?after-before:0;
}
static inline int s9_p4_elapsed_valid(uint64_t before,uint64_t now) {
    return now>before;
}
#endif
