/* SPDX-License-Identifier: MIT
 * P12.2 read-only causal instrumentation helpers.
 * No PMU, PLL, frequency, voltage, frame or scheduler writes.
 */
#ifndef HAKIOS_P122_DIAG_H
#define HAKIOS_P122_DIAG_H
#include <stdint.h>
static inline int s9_p122_mode_valid(int mode) {
    return mode >= 0 && mode <= 2;
}
/* GPU queue / execution latency buckets: <=.25,.5,1,2,4,8,16ms, >16ms.
 * Histogram counters (rather than per-job logs) keep the worker bounded. */
static inline unsigned s9_p122_bucket_us(uint64_t us) {
    static const uint32_t limit[7] = {250,500,1000,2000,4000,8000,16000};
    for (unsigned i=0; i<7; ++i)
        if (us <= limit[i]) return i;
    return 7;
}
#endif
