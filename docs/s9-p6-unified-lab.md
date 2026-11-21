# S9 P6.1–P6.5 unified diagnostics — ONE matching BOOT + SYSTEM image

Branch: `s9/p6-unified-wake-thermal-display-lab` (based on P5 `75cdc6d`).
All P0/P1/P3/P4/P5/G3D implementation retained. No new kernel boot
arguments. P6 enables at runtime using `kern.hakios.p6_*`.
**Nothing in this suite forces CPU_SUSPEND, CPD, SICD, WFI, GPU clocks,
PMIC voltages, scheduler affinity, framebuffer dirty regions or frame skipping.**

## Scope and evidence boundary

| Module | Actual code | Cannot claim |
|---|---|---|
| P6.1 driver sources + wake *proxies* | USB gadget **actual periodic thread-call count**, counts while connected and overlap; S6SY761 **actual touch GIC handler / fallback 4ms poll-check counts**; Wi-Fi bridge **actual GIC handler invocation count**. Existing governor thread: per-core scheduler idle, governor loops, touch/frame notifications, DVFS-worker wake requests. Existing `poster_cost`: process CSW and thread snapshots | No global GIC IRQ, timer-callout origin, WFI, CPU_SUSPEND or core power-off residency. USB gadget/touch/Wi-Fi handlers are specific sources, not total CPU wakeups; haptic driver attribution and global kernel timers are not implemented. Do not label `csw` as IRQ |
| P6.2 background process churn | PID transition and disappearance census for Poster/chronod/SpringBoard/backboardd/known daemons; per-process CPU, context switches, faults, syscalls. Matches abbreviated `PridePoster*` executable names and recognizes launchd-confirmed `PridePosterExtension` SIGSEGV exits offline | PID change is **not** proof of SIGSEGV; only explicit launchd signal messages are treated as confirmed exits |
| P6.3 thermal DVFS/parking **shadow** | Exynos9810 BIG threshold 83/96C, G3D 88/95C, 5C hysteresis; M3 parking *candidate* using LITTLE/BIG load, touch, frame cadence. Same already-sampled TMU, no additional sensor timer | No Samsung IPA, no applied rate, no hotplug; suggested state never changes the governor. Unknown TMU retains prior cap |
| P6.4 G3D shadow and optional real screen-OFF gate | Reuses existing `g3d_lab_gate=1`/2; PMU domain/PLL via existing 5s independent sampler (not GPU MMIO), GPU worker owner, faults and wake latency via P5; actual mode2 requires separate explicit human opt-in | No promise of watt savings. An OFF screen doesn't automatically make PMU G3D OFF. Mode2 can stall on hardware and requires rollback |
| P6.5 CA/EAGL correlation | Per60-frame EAGL wall time/draw/IOSurface sampling, CPU percent of CA display thread via libproc; P5 owner/queue statistics during same timeline | No private CA stack or true timer/vsync origin, no proof unchanged final pixels, no new GL work or frame omission |

Important difference from Jesvi S20: Jesvi's per-core PSCI/MCT/GIC
implementation cannot be copied to Exynos9810. This iteration **does not**
implement deep CPU suspend or global IRQ hooks; they would require changes in
the upstream XNU interrupt/timer sources, absent from this driver overlay.

## Required single build/image

Full P6 telemetry involves **both** kernel and matching userland:
- Kernel: `drivers/power/exynos/hakios.exynos_cpufreq.c` and
  `hakios.p6_lab.h`, `hakios.p6_policy.h`, existing G3D+P5 kernel paths.
- Kernel-specific attribution: `drivers/usb/hakios.dwc3.c` and
  `drivers/input/touch/hakios.s6sy761.c` and
  `drivers/wifi/lx/hakios.lx.c`; six read-only cumulative
  `kern.hakios.p6_usb_*`, `kern.hakios.p6_touch_*` and
  `kern.hakios.p6_wifi_irqs` sysctls.
- System: `userland/src/poster_cost.c` and `userland/src/eagl.c`.
- Host: `tools/s9-p6-serial-auto.ps1`, `tools/s9-p6-report.ps1`.
- Portable CI: `s9-p6-unified-lab-tests.yml`.

For the SIOS GitHub Actions `s9-installer`:
- Workflow ref: `fix/s9-installer-device-selection`.
- `kernel_ref=s9/p6-unified-wake-thermal-display-lab`.
- Keep `metal=true`, `userdata_blocks=starlte=13917184`, `time_limit=0`.
- Keep known working P4/P5 bootargs, including **`poster_probe=1`**,
  `mali_gov=1`, `mali_dvs=0`, `p3power=1`, plus existing P0/P1/P3 graphics settings.
  Do not add p6_* to bootargs; these are runtime sysctls.

**WARNING:** the full installer may FORMAT USERDATA. Never flash it merely to
toggle a sysctl. Verify device/firmware compatibility and off-device BOOT +
SYSTEM + EFS + USERDATA backups and rollback. If current SYSTEM is not the
P6 system, BOOT-only image will not yield `p6-proc`/`p6-ca-gl`.
Only one full matching build/flash is intended; subsequently test all phases
on this build through console commands.

## Controls

`kern.hakios.p6_mode`: 0 off (default), 1 passive, 2 detailed.
`kern.hakios.p6_phase`: 0..99 tag (resets observation windows, hardware untouched).
`kern.hakios.p6_flags`: 0..31 bit mask; 1=CPU/event proxy,
2=process churn, 4=DVFS+parking shadow, 8=G3D correlation,
16=CoreAnimation. Default 31 but **nothing samples until p6_mode>0**.
`kern.hakios.p6_log_ms`: 1000..30000, default 5000.
`kern.hakios.p6_stats`: read-only last CPU/proxy/shadow sample.

P4 and P5 switches remain independent. For G3D bring-up,
`g3d_lab_gate`, `g3d_lab_idle_ms`, `g3d_lab_phase`,
`g3d_lab_stats`, `g3d_lab_reset` are unchanged; **p6_flags=8 NEVER gates GPU**.

P6 output labels:
`p6-wake`, `p6-wake-detail`, `p6-source`, `p6-shadow`,
`p6-proc`, `p6-churn`, `p6-missing`, `p6-thread-snapshot`,
`p6-ca-thread`, `p6-ca-gl`.
P5/P4/G3D labels remain present in the same serial log when enabled.

## One continuous COM125 capture (recommended)

Run a dry-run first (no port opened, no serial writes):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1
```

Then close PuTTY (COM exclusive), start the actual capture:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -Port COM125 -Execute
```

The script requires exact S9 COM identity, exact loader echo **and** sysctl
ACK per command (including `p6_stats` preflight). It captures every raw
COM byte, not only P6 lines. A1 OFF baseline, B1 P6.1, B2 P6.2, B3 P6.3,
B4 G3D shadow+physical OFF/ON, B6 CA static and interactive, A2 OFF
reversal. Each stage is marked with `p6: PHASE`. Keep brightness,
charger, Wi-Fi, wallpaper, ambient, and starting thermal conditions
consistent; do not compare a hot second pass directly to a cold first pass.
Wait >=420s after boot to reduce initial Poster launch noise; nevertheless
record any PridePoster crash events around boot separately.

P6.4 real G3D gate is **NOT** default. Only on a dedicated recovery-ready
device with correct 950mV, no G3D fault, safe MMU/TZ and DVS off, use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-serial-auto.ps1 -Port COM125 -EnableActualG3D -Execute
```

Before mode2, the script re-reads `g3d_lab_stats`, requires mode1
shadow, measured `rail_valid=1 rail_last_mv=950`, requested_mv=950,
`fault=0 domain=1 gpu_safe=1 tz_ok=1` and
explicitly prompts `GATE`. Do not use mode3 here: 10fps still
produces frame activity and power cycling risks. Watch GPU job failures,
queue, PMU domain, PLL, wake latency, battery and thermal. If safety is not
verified, cancel. On any hardware anomaly stop and run safe recovery;
failed reset means boot/rollback.

## Manual on-device phases (do NOT reflash)

Read first:

```text
sysctl kern.hakios.p6_stats
sysctl kern.hakios.g3d_lab_stats
sysctl kern.hakios.p6_mode=2
sysctl kern.hakios.p6_flags=1
sysctl kern.hakios.p6_phase=2
```

For process churn with event proxies:
```text
sysctl kern.hakios.p6_flags=3
sysctl kern.hakios.p6_phase=3
```

For shadow policy, no clock writes:
```text
sysctl kern.hakios.p6_flags=5
sysctl kern.hakios.p6_phase=4
```

For screen OFF G3D *shadow* plus P5 GPU telemetry:
```text
sysctl kern.hakios.p6_flags=9
sysctl kern.hakios.p6_phase=5
sysctl kern.hakios.p5_mode=2
sysctl kern.hakios.p5_phase=5
sysctl kern.hakios.g3d_lab_idle_ms=10000
sysctl kern.hakios.g3d_lab_gate=1
sysctl kern.hakios.g3d_lab_rail_probe=1
sysctl kern.hakios.g3d_lab_phase=5
```

Then physically turn display OFF 90–120s, turn ON, read
`sysctl kern.hakios.g3d_lab_stats` and `p5_stats`.

Only if consciously testing **actual** G3D gating with the documented
device safety prerequisites met, switch `g3d_lab_gate=2` while
screen ON, then physically OFF. Check that PMU domain becomes 0 while OFF,
and returns to 1 before resumed jobs after physical wake. To exit:
```text
sysctl kern.hakios.g3d_lab_gate=0
sysctl kern.hakios.g3d_lab_reset=1
sysctl kern.hakios.g3d_lab_rail_probe=0
sysctl kern.hakios.g3d_lab_stats
```
Note: `g3d_lab_reset=1` **writes and verifies 950mV**; it is a safety
recovery, not a diagnostic no-op.

For CA / EAGL work (takes ~60 completed GL frames for one group):
```text
sysctl kern.hakios.p6_flags=17
sysctl kern.hakios.p6_phase=7
sysctl kern.hakios.p5_mode=2
sysctl kern.hakios.p5_threads=1
sysctl kern.hakios.p5_phase=7
```

Safe return to baseline:
```text
sysctl kern.hakios.p6_mode=0
sysctl kern.hakios.p5_mode=0
sysctl kern.hakios.p5_threads=0
sysctl kern.hakios.p4_mode=0
sysctl kern.hakios.p6_phase=8
sysctl kern.hakios.p6_stats
```
**Important:** disabling `p6_mode` does NOT disable a separately enabled
`g3d_lab_gate=2`. Always exit G3D before the other lab settings.

## Offline analysis

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p6-report.ps1 -Log putty-P6-unified-YYYYMMDD-HHMMSS.log
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s9-p5-report.ps1 -Log putty-P6-unified-YYYYMMDD-HHMMSS.log
```

`p6-source` includes absolute counter **deltas**, `present_mask` and
`delta_valid` masks (bits 1,2,4,8,16,32 correspond to USB tick,
connected tick, overlap tick, touch IRQ, touch poll and Wi-Fi IRQ). A missing
counter or first sample is invalid, not zero wakeups. USB serial capture
itself keeps the USB gadget connected and adds scheduling overhead:
USB poll counts from a COM125 run do not represent unplugged battery idle.
The S9 touch driver might be configured in IRQ mode (touch IRQ counter)
or fallback 4ms polling mode (touch poll count), but these cannot be
converted into real per-core power-state wakeups.

P6 parser tolerates missing/garbled serial records; it doesn't repair
raw interleaved lines or treat them as valid. It prints per-phase wake
proxies, suspected PID churn, launchd-confirmed PridePoster SIGSEGV exits, shadow decisions, G3D PMU OFF sample counts,
GPU errors, CA rendering proxies, and raw crash-related strings.

## Test and rollback rules

Run portable policy tests and source checks in CI workflow
`s9-p6-unified-lab-tests.yml`. Windows CI parses both PowerShell scripts,
runs an inert COM preview (including actual-gate opt-in preview only)
and a synthetic report with intentionally broken interleaved P5 lines.
Do NOT interpret portable tests as proof a kernel binary boots on real S9.

Stop immediately for thermal runaway, missing GPU MMU/TZ verification,
job errors, GPU domain partial, blank screen or corrupt frames. Disabling
P6 passive sampling doesn't undo power changes. Keep existing working BOOT+
SYSTEM rollback image available at the bench. No CPU deep-idle writes,
voltage tuning or new framebuffer optimizations are in P6.
