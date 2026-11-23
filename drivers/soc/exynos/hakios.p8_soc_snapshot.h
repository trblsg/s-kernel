/* P8 shared snapshot ABI. Read-only; no PMU, CPU or PSCI power state writes.
 * One source copied to BSD/dev by drivers/map and to osfmk/arm64 by
 * the iOS 16.5 fork_copy profile; used by M3 driver, PSCI and governor.
 * UNKNOWN is explicit: a successful XNU processor_exit is never proof of
 * PSCI AFFINITY_OFF or PMU hardware OFF.
 */
#ifndef HAKIOS_P8_SOC_SNAPSHOT_H
#define HAKIOS_P8_SOC_SNAPSHOT_H
#include <stdint.h>
#define HAKIOS_P8_M3_CPUS 4u
struct hakios_p8_m3_snapshot {
    uint32_t valid;
    uint32_t cpu_config[HAKIOS_P8_M3_CPUS];
    uint32_t cpu_status[HAKIOS_P8_M3_CPUS];
    uint32_t noncpu_config, noncpu_status, l2_status;
    uint32_t reset_sequence, reset_disable_soft, reset_disable_wdt;
};
struct hakios_p8_rails_snapshot {
    uint32_t valid, lit_khz, big_khz;
    uint8_t b2_ctrl, b2_vsel, b3_ctrl, b3_vsel;
    int32_t err_lit, err_big, err_b2c, err_b2v, err_b3c, err_b3v;
};
struct hakios_p8_psci_snapshot {
    uint32_t logical_cpu, mpidr, stage, start_attempts, off_attempts, off_returns;
    int32_t last_cpu_on_status, last_cpu_off_status;
    int64_t affinity;
    uint32_t valid;
};
/* Called only in an explicit P8 diagnostic/read-only sampler context. */
int hakios_p8_m3_snapshot(struct hakios_p8_m3_snapshot *out);
int hakios_p8_rails_snapshot(struct hakios_p8_rails_snapshot *out);
int hakios_p8_psci_snapshot(unsigned cpu, struct hakios_p8_psci_snapshot *out);
#endif
