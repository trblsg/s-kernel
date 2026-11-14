# Screen-off power on the Galaxy S9

Status: built; first run with the screen off on 13 November 2026 (kernel `gea3e2419`). It found `sleep_cores=1` a
no-op on the S9: the loader's `/cpus` nodes have no `cluster-type`, so the fork's `sleepCoreWanted()` (which took only
`CLUSTER_TYPE_P`) chose no core, and all four M3s stayed online. `xnu/files/GenericARMPE.cpp`, copied over the fork's
file by `profiles/ios-16.5.copy`, now falls back to `small-core`/`l2_access_penalty`; the cpufreq governor skips a
core whose bit is in `hakios_cpu_exit_mask`, so an off core is not counted as 100% busy. Everything here is behind
boot-args that are off by default.

## What happens today when the screen goes off

- **No system sleep.** `IOPMrootDomain` refuses it (`gSleepDisabledFlag = 1`; `syssleep=1` gives Apple's behaviour back):
  with nothing to sleep into and nothing to wake from, powerd's sleep request ended in `IOCPUSleepKernel()` and never
  came back. The phone stays awake with the screen off; the side button or a tap wakes the screen.
- **The cores idle in WFI** (`cpu_idle()`, `osfmk/arm64/cpu.c`): a clock gate only. Nothing asks the firmware for a
  core power-down (PSCI `CPU_SUSPEND`) or a cluster power-down, so every running core keeps its supply and leaks.
- **The display is not told.** backboardd sends its power request to the display service (IOMobileFramebuffer selector
  12, one scalar: seen at boot as 1, 0, 1), and `MiniIOMobileFramebuffer.cpp` answered "done" and did nothing. The
  screen goes dark by its brightness alone: the S6E3HA8 keeps scanning in display-on, the DSIM and DECON stay up.
- **The front camera** (`cam_s9=7`) no longer streams from boot: since `cc2b18f9` it starts at the first read of
  `/dev/hakios-camera` and stops after the last close or `STREAM_IDLE_MS` without a read (`cam_s9_always=1`: the old
  behaviour). Its power cost with and without a reader is still to be measured.
- **Modem**: parked (not started), nothing to do.

## What this adds (`GenericARMPE.cpp`, `MiniIOMobileFramebuffer.cpp`, `gic_v3_irq.c`, `psci_smp.c`, patch 1517, `hakios.s6e3ha8.c`)

| Boot-arg | Default | What it does |
|---|---|---|
| (none) | | every change of the display power request is logged: `GenericARMPE: the display off (iOS's power request)` |
| `panel_sleep=1` | off | with `panel=1`: screen off sends display-off (28) and sleep-in (10) between the level-1 key, as Samsung's display-off and exit sequences do; screen on sends sleep-out (11), the last brightness again, display-on (29). The supplies stay on, so the panel keeps what the boot loader set it to (DSC, PPS, TE): whether it all survives a sleep-in is the first thing the phone run shows |
| `sleep_cores=1` | off | `sleep_cores_after=` seconds (10) after the screen went off, the performance cores are taken off with `processor_exit()` (each ends in PSCI `CPU_OFF`; with all four off the firmware may power the cluster down); at screen-on they are started again with `processor_start()` (PSCI `CPU_ON`, the boot's path). The performance cores are `CLUSTER_TYPE_P`, or, on a tree with no `cluster-type` (the S9's), the ones not marked `small-core` (`l2_access_penalty`). `sleep_cores=2`: every core but the boot processor and the loader's. Never the boot processor, never the loader's core (`core0=1`) |

Kernel pieces of `sleep_cores=` (patch 1517 for Apple's `cpu_common.c` and `cpu.c`; our own files for the rest): `ml_cpu_can_exit()` (`osfmk/arm/cpu_common.c`) allows exactly the
cores whose bit GenericARMPE has set in `hakios_cpu_exit_mask` (without `sleep_cores=` it stays 0 and `processor_exit()`
is refused as before); `cpu_sleep()` (`osfmk/arm64/cpu.c`) sets the sleep token that `cpu_exit_wait()` waits for, which
only Apple's path set; `psci_smp_start_cpu()` waits for `AFFINITY_INFO` to say "off" before a second `CPU_ON` of a core;
`gic_v3_spi_move()` routes the core's device interrupts to the boot processor before it goes (an SPI is routed to the core
that attached it), and the core's GIC state is forgotten at its shutdown so that `gic_v3_irq_cpu_init()` sets it up again
at its return.

## How to measure (read-only)

`max77705=1 max77705_fg_s=30` logs the gauge every 30 s: RepSOC, RepCap, Current, AvgCurrent and the raw coulomb counter
QH; `max77705_diag=1` adds the charger's own registers (0xB2-0xC5) to every 5-minute line. Nothing is written to the
MAX77705. With the phone on USB the charger feeds the system, so the battery's current shows the load only while the
input is the limit (or with the cable out): compare runs under the same supply, screen off 30 minutes each:

1. baseline: today's image, screen off by the side button;
2. `panel_sleep=1`;
3. `panel_sleep=1 sleep_cores=1`;
4. `cam_s9_always=1` against the default, for the camera's share.

What to look for in the log besides the numbers: `s6e3ha8: panel_sleep:` lines (packets sent), the picture after the
screen comes back, `GenericARMPE: sleep_cores: cpu N taken off` / `started again` with their times, and
`psci: cpu N: started again; AFFINITY_INFO`.

## Not done (next steps)

- Core power-down in idle (PSCI `CPU_SUSPEND` from `cpu_idle()`, waking through the reset vector as Apple's deep idle
  does) and the SoC's own idle states through ACPM: bigger, and riskier than taking cores off while nothing runs.
  What the phone's device tree says (`exynos9810-starlte_eur_open_26.dts`, `idle-states`, read for these numbers):
  entry by PSCI; one core state, "c2", `arm,psci-suspend-param` 0x10000 on both clusters; the boot (little) cluster
  70 us in, 160 us out, worth it from 2 ms idle; the big cluster 235 us in, 220 us out, from 3.5 ms. The cluster
  power-down (CPD) and the system's idle clock-down (SICD, through ACPM) are on with 10 ms residencies
  (`cpd_enabled`, `sicd_enabled`, `exynos-powermode`). A first step that needs no new wake path: `CPU_SUSPEND` with
  0x10000 only when the next timer is more than the residency away, the core's context saved as `cpu_sleep()` saves
  it and restored through `LowResetVectorBase` as a `CPU_ON` start is (the code that patch 1517 exercises), behind a
  boot-arg, measured against `sleep_cores=`.
- The DECON and DSIM clocks while the panel sleeps; the touch controller's scan rate (tap-to-wake costs a scanning
  controller); the frame thread's behaviour with a sleeping panel (no TE).
